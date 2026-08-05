# Audio — CPU cost of software synthesis on SH-4, and the sm64-dc precedent

§2 and §8 of `kb/audio-plan.md`, moved verbatim: the derived per-voice operation
counts, the SH-4 cycle model with its 80–180 cyc error bars, the A0–A4 config
table, and the comparison against sm64-dc/SM64. Read when sizing the audio
budget or arguing about whether stage A fits.
**Parent verdict, unchanged: audio is a real risk, not a solved problem**
(`kb/audio-plan.md`). Everything here is **[modelled]** except where marked.

---

## ⚠️ 0. TWO CORRECTIONS THAT MOVE EVERY NUMBER BELOW (2026-08-05)

Read these before quoting anything in §2.

### 0.1 A jaudio DAC frame is 17.49 ms of audio, not ~35 ms — MEASURED

`dc/src/dc_audio.c:53-55, :129-131, :139-141, :916` all say "~35 ms of audio",
and `kb/STATE.md` was costing the audio budget against it. **It is 17.49 ms.**
`aictrl.c:292` passes `AIInitDMA(..., DAC_SIZE * 2)` where `DAC_SIZE` is in
**s16 units**, so `DAC_SIZE * 2` is 2,240 **bytes** = 1,120 s16 = 560 stereo
pairs; at `JAC_DAC_RATE = 32028.5` (`internal/rate.c:4-7`) that is 17.49 ms.
`kb/audio-engine.md`'s parameter table has had it right all along — 57.19 Hz.

**Consequence: at the measured ~19.8 ms of SH-4 per DAC frame, synthesis runs at
0.88× real time and needs ~113 % of the machine to stay level — not 1.8× at
~57 %.** Corroborated by `dc_audio.c:120-122`'s own note that the ring "starves
essentially always", which is impossible above real time.

### 0.2 The 80-180 cyc/voice-sample band is OPTIMISTIC — `rspsim.c` is `-O0`

§2.2's model assumes ordinary compiled code. `src/static/jaudio_NES/internal/
rspsim.c` is in `src/`, so it compiles at **`-O0`** like everything else
(CLAUDE.md §1), with a stack round-trip per intermediate. Back-solving the
cycle count from the 0.88× real-time figure in §0.1 gives **~200+
cyc/voice-sample**.

**⇒ every row of §2.3 needs scaling by ~1.7×.** A1 34 % → **~57 %**; A4 13 % →
**~22 %**. Nothing in the A0-A4 table fits the ≤12-18 % budget §2.3 derives.
That does not change the plan of record — stage B was already assumed
(`kb/audio-plan-of-record.md`) — but it does remove "A4 fits" as a fallback.

---

## 2. CPU cost — method, numbers, error bars

### 2.1 Method

The rspsim hot loops have **fixed trip counts** (the only data-dependent inner
loop is the resampler's input-advance `while`, whose average iteration count is
the pitch ratio). So the dynamic operation counts are *derived*, not sampled.
Per output sample, per voice, for the default chain (VADPCM decode → 4-tap
polyphase resample → envelope/pan mixer):

**`A_CMD_ADPCM` (`rspsim.c:107-213`)** — per 16-sample VADPCM frame (9 bytes):
- nibble expansion: 8 byte loads → 16 table lookups + 16 stores
- two 8-sample halves; half *k* does `2+k` multiply-accumulates ⇒
  Σ(2+k), k=0..7 = **44 MACs per half, 88 per 16 samples**
- 16 shifts, 32 clamp compares, 16 stores
⇒ **≈ 5.5 multiplies + ≈ 14 other ops per sample.**
All 1157 samples in the game use `order = 2, n_predictors = 2` **[measured]**,
so this count is exact, not typical.

**`A_CMD_RESAMPLE` (`rspsim.c:222-293`)** — per output sample: 4 multiplies,
3 adds, 1 shift, 2 clamps, 1 store, ~4 loads, 1 table index, plus the
input-advance loop (~1 iteration × 5 ops at unity pitch)
⇒ **≈ 4 multiplies + ≈ 13 other ops per sample.**

**`A_CMD_ENVMIXER` (`rspsim.c:456-589`)** — per 2 samples: 4 multiplies for the
dry pair, then four `((v*e)>>16) + ((v*e)>>18) + ((v*(u16)e)>>19)` expressions
= 12 more multiplies, ~14 adds, 12 shifts, 16 clamps, 8 stores, 4 loads
⇒ **≈ 8 multiplies + ≈ 27 other ops per sample**, *plus* the `f32` round-trip:
`var_f2/f4/f5/f6` are declared `f32` and immediately re-truncated to `s32`
(`driver.c` matching artifact) ⇒ **4 int↔float conversions per sample**.

**Total core chain: ≈ 17.5 multiplies + ≈ 54 other integer ops + 4 FP
conversions, per voice, per internal sample.**

### 2.2 SH-4 cost model **[modelled]**

SH-4 @200 MHz, dual-issue in-order, `MUL.L`/`DMULS.L` issue rate 2 cycles,
latency 4; 8 KB I$ / 16 KB D$.

| term | cycles |
|---|---|
| 17.5 multiplies × 2 cyc issue | 35 |
| 54 other ops ÷ 0.75 sustained IPC | 72 |
| 4 FP conversions (`LDS`+`FLOAT` / `FTRC`+`STS`) × ~5 | 20 |
| **central estimate** | **≈ 127 → call it 120** |

**Honest range: 80–180 cycles per voice-sample.**
- 80 = FP round-trip removed (−20), `MAC.W` used for the ADPCM/FIR taps, IPC ~1.0
- 180 = GCC scheduling this gnu89 pointer-chasing code badly, plus D-cache
  pressure from 24 live `channel`/`driverch` structs

**Cache is a favourable, not adverse, factor:** DMEM is a 4 KB static array,
the codebook is 64 B, `RES_FILTER` is 512 B, and sample bytes arrive through
0x400-byte waveload buffers — the whole hot working set is well under the
16 KB D-cache. Sample-data misses are ~1 line per 57 samples (9 VADPCM bytes
per 16 samples vs a 32 B line) ≈ +0.5 cycles/sample. Included in the range.

**Global per-update fixed cost** (reverb line, final interleave/resample,
buffer clears): ≈ 21,000 cycles/update ≈ 84,000 cycles/audio frame
≈ 4.8 M cycles/s ≈ **2.4% CPU** at 57.19 Hz. Reverb dominates it (~15k of the
21k). Per cycle spent it is the *best-value* effect in the engine — see §4.

### 2.3 Results

Voice-samples/s = voices × internal rate. CPU% = (voice-samples/s × cyc) / 200e6.

⚠️ **Every CPU% in this table is at the 120 cyc/voice-sample central estimate
and must be scaled by ~1.7× — see §0.2.** The scaled column is what to plan
against.

| config | out / internal rate | voices | central | range | **×1.7 (`-O0`)** |
|---|---|---|---|---|---|
| **A0** as-shipped | 32.03 k / 45.76 k | 24 | **68%** | 46–101% | **~116%** |
| A0 typical load (guess 12 voices) | 32.03 k / 45.76 k | 12 | 35% | 24–52% | ~60% |
| **A1** half-rate | 22.05 k / 22.97 k | 24 | **34%** | 23–51% | **~57%** |
| **A2** half-rate, capped, FIR+comb off | 22.05 k / 22.97 k | 16 | **21%** | 14–32% | ~36% |
| **A3** A2 + FP round-trip removed | 22.05 k / 22.97 k | 16 | 18% | 12–27% | ~31% |
| **A4** budget-fitting | 22.05 k / 22.97 k | **10** | **13%** | 9–20% | **~22%** |

**The budget arithmetic that matters.** At 30 fps the frame is 33.3 ms. PLAN
§3.2's M3 gate allows game logic ≤25 ms. That leaves ≤8.3 ms for *everything
else* — renderer submission, T&L, texture work, disc I/O — so audio must land
at **≤4–6 ms/frame ≈ 12–18% CPU**. ⚠️ **Corrected 2026-08-05: at the `-O0`
scaling of §0.2, NOTHING in the table fits** — A4, the cheapest config, is
~22 %. This sentence used to read "Only A4 (and A3 at ~10–12 voices) fits."
**Stage A does not fit the plan's own budget at any voice count in this table.**

The DC has one core; the "audio thread" is time-slicing, not parallelism. 34%
CPU is 11.3 ms taken straight out of the 33.3 ms frame.

### 2.4 Optional per-voice effect adders **[modelled]**

| effect | gate | added cyc/voice-sample | note |
|---|---|---|---|
| FIR filter (8-tap, `A_CMD_FIRFILTER`) | `common->filter != 0`, set by sequence cmd (`sub_sys.c:589`) | +32 (+27%) | 8 MAC + shift + clamp |
| comb filter | `comb_filter_size && comb_filter_gain`, sequence cmd (`sub_sys.c:572`) | +12 (+10%) | 3 block copies + 1 mix |
| Haas | `sound_mode == HEADSET` | n/a | **off by default** |
| Dolby surround | `sound_mode == DOLBY_SURROUND` | n/a | **off by default** |
| gain/dist filter | `common->gain != 0` | 0 | already a no-op |
| velocity convolution | always | 0 | already a no-op |

What fraction of live voices set `filter`/`comb_filter_size` is **unmeasured** —
it comes from sequence opcodes, not from static bank data. See §7.

---

## 8. Precedent — sm64-dc and friends

**Verified:** `jnmartin84/sm64-dc` keeps the full N64 software audio engine —
`src/audio/{synthesis.c, playback.c, seqplayer.c, load.c, effects.c, heap.c}`
are all present, `synthesis.c` at 64 KB. Its platform backend
(`src/pc/audio/audio_dc.c`) is a double-buffered 1088-sample stereo feed
defined at 32 kHz. So the precedent for "N64 software synthesis at full speed
on SH-4" is real and it is *software* synthesis, not AICA voices.

**Verified from `n64decomp/sm64` `src/audio/data.c`:** SM64's session presets
run at **32,000 Hz with 8–20 simultaneous notes and 1 reverb**.

**Comparison (voice-samples/s, the scaling quantity):**

| | rate | voices | voice-samples/s | vs SM64 |
|---|---|---|---|---|
| sm64-dc (worst preset) | 32,000 | 20 | 640 k | 1.00× |
| **AC as-shipped (A0)** | 45,755 | 24 | **1,098 k** | **1.72×** |
| AC stage A1 (22 kHz) | 22,968 | 24 | 551 k | 0.86× |
| AC stage A4 (22 kHz, 10 voices) | 22,968 | 10 | 230 k | 0.36× |

**What is heavier in AC than in SM64, itemised:**
- internal mixing rate **45.75 kHz vs 32 kHz** (1.43×) — the biggest factor,
  and the one nobody would guess from the "32 kHz output" label
- **24 voices vs 20** (1.2×)
- **per-voice FIR filter** (`LSF_TABLE`/`HSF_TABLE`) — SM64 has none: +27% on
  affected voices
- **per-voice comb filter** — SM64 has none: +10% on affected voices
- **reverb with a sub-delay line** (`sub_delay = 20`) — SM64's reverb is the
  simple single line: roughly +50% on the reverb block, but that block is only
  ~1.7% CPU
- **Haas and Dolby surround** — SM64 has neither, but **both are off in AC's
  default STEREO mode**, so they do not count against us
- the **`f32` round-trip in ENVMIXER** — an artifact of *this* decomp, not
  present in SM64's integer envmixer: +16%, and removable

Net: at shipped settings AC's audio is **~1.7–2.2× sm64-dc's**; at 22 kHz with
the default sound mode and the FP fix it is **~0.8× of it** — i.e. inside the
envelope that is already proven to run at full speed on real hardware.

**Counter-evidence, and why stage B is still the plan:** the Diddy Kong Racing
Dreamcast port (Girgis/jnmartin, public update 2026) reported the game "dipping
to below 20 FPS with audio cutting in and out as the main thread couldn't keep
up with the audio decoding while simultaneously handling rendering and game
logic." Software N64 audio on SH-4 is *feasible* but it is a first-class
consumer of the frame budget, and AC's renderer will be heavier than SM64's.

Sources: [sm64-dc](https://github.com/jnmartin84/sm64-dc) ·
[n64decomp/sm64 data.c](https://raw.githubusercontent.com/n64decomp/sm64/master/src/audio/data.c) ·
[DKR-DC status](https://x.com/falco_girgis/status/2079522376950170101) ·
[dcaconv](https://github.com/TapamN/dcaconv)
