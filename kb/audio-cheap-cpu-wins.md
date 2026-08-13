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

## 🔴 W1 — CLOSED (2026-08-13). IT DOES NOT FIT ARAM.

🔴🔴 **DEAD. DO NOT REBUILD IT. `kb/closed.md`.** Built, wired, run, listened to
("sounds like dogshit"), diagnosed, and closed on arithmetic: the full S8 image
needs **+4,839,936 B** of audio ARAM that the graph half cannot spare
(`forest_2nd.arc` alone is 4,132,608 B), and a mixed bank inside the 104,608 B
of existing headroom converts **2.00 %** of the bank. The codec was never the
problem — 8-bit round-trip SNR is a median **38.56 dB** against the VADPCM's
14-44 dB.
⭐ **The audio CPU win goes to the AICA offload instead**, whose samples live in
the Dreamcast's own 2 MB sound RAM and never touch this budget.
Everything below is kept as the record of how it failed.

## 🔴 W1 — SWITCH THE BANK TO `CODEC_S8` — BUILT, RUN, AND **BROKEN**

**~29 % of `RspStart` ≈ 5.5 % of busy ≈ 4.2 ms/frame** in principle, for an
offline data conversion and zero lines of runtime code.

🔴🔴 **STOP: AS BUILT ON 2026-08-13 IT SOUNDS BAD ON A HUMAN LISTEN, AND THE
CAUSE IS KNOWN.** The audio ARAM reservation is a hardcoded `0x810000` that
does not scale with the image, so graph ARAM overwrites the top ~4.8 MB of the
enlarged audiorom. **Read "W1 IS BROKEN AS BUILT" below before doing anything
in this section.** The codec is fine; the address-space budget is not.

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

### Quality — the codec is fine; see the ARAM bug above for what actually broke

⭐ **MEASURED, 60 real samples**: 54 of 60 peak above 16,000 of 32,767 (median
32,393) and the 8-bit round-trip SNR is a median **38.56 dB**, against the
14–44 dB measured for the VADPCM it replaces. **8-bit quantisation is not what
made the build sound bad.**

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

### 🔴🔴 W1 IS BROKEN AS BUILT — A HUMAN LISTENED AND IT SOUNDS BAD (2026-08-13)

**Standing verdict, and it overrides everything in this section: *"sounds like
dogshit"*.** Read the rest of this section knowing that.

🔴 **THE +8.6 % FPS BELOW IS CONTAMINATED. DO NOT QUOTE IT AS W1'S RESULT.**
It was measured on a build in which most of wave bank 5 was garbage.

**THE BUG — and it is NOT the codec.** Measured on 60 real samples: 54 of 60
peak above 16,000 of 32,767 (median peak 32,393), and the 8-bit round-trip SNR
is a median **38.56 dB** — *better* than the 14–44 dB the VADPCM it replaces
measures. 8-bit quantisation was never the problem.

The problem is **ARAM address space**, and the evidence was in the A/B table
below the whole time:

```
[DC/ARAM] audio half = [0, 8454144)      <- IDENTICAL in both builds
aram_mapped   baseline 13,282,784        S8 18,227,328   (ARAM is 16,777,216)
```

`JKRAram::JKRAram` (`JKRAram.cpp:35-51`) takes the audio reservation as a
**caller-supplied constant** and ARAllocs it first:

```cpp
mAudioMemorySize = bufSize;                      // 0x810000 = 8,454,144
mAudioMemoryPtr  = ARAlloc(mAudioMemorySize);    // the first ARAlloc
mGraphMemoryPtr  = ARAlloc(mGraphMemorySize);    // graph starts at 8,454,144
```

`bufSize` is hardcoded `0x810000` at **`src/static/jsyswrap.cpp:487`** and
**`src/static/jaudio_NES/game/game64.c_inc:1857`**. It is sized for the stock
8,300,384 B audiorom. **It did not scale with the 13,244,928 B S8 image**, so
graph/user ARAM begins *inside* the audiorom and overwrites everything above
8,454,144 — which is most of wave bank 5. Those instruments play graphics data.

⚠️ **AND THE NAIVE FIX DOES NOT FIT.** Raising `bufSize` to cover 13.2 MB
leaves `16,777,216 − 13,271,040 = 3,506,176 B` for graph+user, and
`forest_2nd.arc` alone is **4,132,608 B**. The S8 bank at 1.78× does not fit
the GameCube's 16 MB ARAM address space alongside the graphics. **That is a
hard constraint this document missed.**

### The two ways out, neither tried

1. **Raise `DC_ARAM_SIZE`.** The 16 MB is the *GameCube's* ARAM; on this port
   ARAM is an emulated, disc-backed address space with only a 1 MB resident
   window (`DC_ARAM_BLOCK_SIZE` 32,768 × `DC_ARAM_MAX_BLOCKS` 64), so the
   address space is ours to choose and costs almost no RAM to widen. Needs
   `DC_ARAM_SIZE` raised in `dc/` **plus** shrink rules for the two `0x810000`
   constants, since `JKRAram` derives graph size as `aramSize − bufSize`.
   ⚠️ `jsyswrap.cpp` already carries one of the project's five `TARGET_DC`
   branches — check before adding a sixth.
2. **A MIXED bank.** The codec is per `smzwavetable`, so converting only some
   samples is legal and needs no extra machinery. Convert only enough to stay
   inside the existing 8,454,144 B budget, choosing by CPU-per-byte. Gets a
   fraction of the win with **zero** layout risk.

### ⚠️ What this cost, so the lesson survives

**Four counters were green and the build was broken.** `S8 bank OK`,
`ASSET MISSING 0`, zero asserts, and `[NEOS_OUT]` peak 5807 vs the baseline's
5806 — that last one was read here as *"the strongest cheap evidence the
conversion is correct"*. **It is not.** Peak amplitude is dominated by whichever
voices are loudest; it cannot see a subset of instruments playing noise. The
`aram_mapped` figure exceeded the ARAM address space **in the very table that
was used to declare success**, and it was read past.

**The only instrument that found this was a human listening for ten seconds.**

### The (contaminated) A/B, kept for its ARAM evidence

Matched A/B, `DC_AUDIO_S8=1` vs `=0`, everything else identical, 240 s each at
the title screen's live demo (which has actors, camera **and music**, so audio
is genuinely exercised):

| | baseline | **S8** | |
|---|---:|---:|---|
| **FPS p50** | 23.2 | **25.2** | **+8.6 %** |
| frames in 240 s | 4,019 | **4,349** | +8.2 % — an independent confirmation |
| `[STUTTER]` | 156 | **122** | fewer |
| `ASSET MISSING` | 0 | 0 | ✅ |
| asserts / panics | 0 | 0 | ✅ |
| `[NEOS_OUT]` max peak | 5806 | **5807** | same audio, same level |
| `aram_mapped` | 13,282,784 | 18,227,328 | +4,944,544 = exactly the image growth |

⭐ **The boot guard fired green: `[DC/AUDIO] S8 bank OK (13244928 B)`** — the
image and the linked tables agree.

🔴 **THE `[NEOS_OUT]` 5807-vs-5806 ARGUMENT WAS WRONG AND IS RETRACTED.** It
was read here as proof the conversion was correct. Peak amplitude is set by
whichever voices are loudest and **cannot see a subset of instruments playing
noise** — which is exactly what was happening. A near-identical peak is
consistent with a badly broken bank.

⚠️ **`run_report.py --vs` says REGRESSED and it is the documented false
positive.** The three counters it names — `emu_punt`, `emu_vid_batches`,
`emu_pdec` — track **where the camera is standing**, exactly like
`pvr_dropped` (`kb/RESUME.md` §9). `emu_trin` fell 13,832 → 10,370 between the
two runs, i.e. the final 30-frame window simply had less geometry on screen.
Two runs of a looping demo do not sample the same moment. Do not read it as a
renderer regression, and do not "fix" it.

⚠️ **Flycast UNDERSTATES this.** The hardware profile put audio at **2.13× its
Flycast share**, so +8.6 % here is a floor for silicon, not a ceiling. The
converse of measurement rule 12.

Artefacts on the NAS: `AC-DC-20260813-s8.cdi`, `AC-DC-20260813-s8base.cdi`
(the matched pair), `audiorom-s8.img`.

### What is STILL not verified

🔴 **Nobody has listened, and no counter can substitute.** Every check above is
amplitude and liveness; none of them can hear the thing this change actually
risks — 8-bit quantisation noise is *constant* where ADPCM's is
signal-proportional, so the failure mode is hiss in quiet sustained passages,
at full `[NEOS_OUT]` peak and zero errors. **Play `AC-DC-20260813-s8.cdi`
against `AC-DC-20260813-s8base.cdi` before burning either.**

🔴 **Only the title demo has run.** The town has more voices and a different
soundfont mix. The FPS number is the title's.

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
