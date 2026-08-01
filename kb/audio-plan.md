# Audio plan — jaudio_NES on Dreamcast

Written 2026-08-01 (measure pass). Supersedes the sketch in `PLAN.md` §3.4.

**Verdict up front: audio is a real risk, not a solved problem.** Software
synthesis (stage A) is *buildable* and is the right bring-up path, but at the
game's shipped settings it costs an estimated **~68% of the SH-4** and even at
22 kHz with effects trimmed it costs **~34%** at the full 24-voice cap — which
does not fit alongside PLAN's ≤25 ms game-logic gate at 30 fps. Plan on
stage B (AICA hardware voices) being **required**, not a fallback. The good
news, all newly measured: the bank format converts cleanly, the *per-sequence*
sample working set is tiny (median 49 KB, 247 of 249 sequences use exactly one
soundfont), and the two most expensive per-voice effects are already off in the
game's default sound mode.

Everything below marked **[measured]** was derived by parsing the real
`audiorom.img` out of the user's GAFE01 ISO and the real engine source.
Everything marked **[modelled]** is an arithmetic estimate with stated error
bars — no SH-4 hardware or toolchain existed when this was written.

---

## 1. What the engine actually is

`src/static/jaudio_NES/` is **not** JAudio/IBNK despite the `Bank_Test`/`IBNK`
code in `bankread.c` (that path is unused by GAFE01 — see §3). The live engine
is the **N64 libaudio synthesis driver of the Zelda MM / SM64 lineage**,
retargeted to GameCube, with the RSP audio microcode replaced by a C
interpreter (`internal/rspsim.c`).

Call chain, per audio frame:

```
Jac_VframeWork → MixCpu (cpubuf.c)  → Neos_Update (neosthread.c)
  → CreateAudioTask (sub_sys.c)     → Nas_smzAudioFrame (driver.c:171)
        ├─ Nas_MySeqMain(u)         ×4   sequencer (track.c/jammain_2.c) — cheap
        ├─ __Nas_PushDrvReg(u)      ×4   snapshot voice state → AG.common_channel[]
        └─ Nas_DriveRsp(...)        ×4   emit RSP command list  ← THE SEAM
              └─ Nas_SynthMain(...) ×voices  per-voice command emission
  → RspStart2/RspStart (rspsim.c)   interpret the command list = ALL the DSP work
  → AIInitDMA(dac, DAC_SIZE*2)      (aictrl.c) → pc_audio.c ring → SDL
```

### Shipped parameters **[measured]**

Source: `internal/rate.c`, `internal/audioconst.c` (`NA_SPEC_CONFIG[0]`),
`internal/memory.c:__Nas_MemoryReconfig`, `include/jaudio_NES/system.h`.

| parameter | value | where |
|---|---|---|
| output DAC rate | 32,028.5 Hz stereo | `JAC_DAC_RATE` |
| output samples / audio frame | 560 stereo pairs (`DAC_SIZE`=1120 s16) | `JAC_FRAMESAMPLES` |
| audio frame rate | 32028.5 / 560 = **57.19 Hz** | derived |
| internal mixing rate (`spec->_00`) | 0xBB80 = 48,000 nominal | `NA_SPEC_CONFIG` |
| samples/frame target | 48000 / 60 = 800 | `refresh_rate`=`REFRESH_RATE_NTSC`=60 |
| updates per frame | 4 | hardcoded in `__Nas_MemoryReconfig` |
| samples per update | (800/4) & ~7 = **200** | `num_samples_per_update` |
| **effective internal rate** | 4 × 200 × 57.19 = **45,755 Hz** | derived |
| **max simultaneous voices** | **24** (`spec->_05`, `AGC.maxChan`=0x18) | `AG.num_channels` |
| synth reverbs | 1 (`spec->_09`), `NA_HALL_DELAY` | `AG.num_synth_reverbs` |
| reverb delay line | `_02`=0x20 → 0x20×0x40 = **2048 samples** (8 KB L+R) | `Nas_SetDelayLineParam` case 1 |
| reverb sub-delay | 20 (non-zero ⇒ second load/save/mix path active) | `NA_HALL_DELAY.sub_delay` |
| default sound mode | `SOUND_OUTPUT_STEREO` | `system.c:1526` |
| max ABI cmds | 24×20×4 + 1×30 + 400 = 2350 | `AG.max_audio_cmds` |
| DMEM | 4 KB static (`rspsim.c:DMEM[0x1000]`) | fits SH-4's 16 KB D-cache |

Note the final resampler: `A_CMD_INTERLEAVE` calls `Jac_Resample16` which
converts each update's 200 internal samples → `JAC_FRAMESAMPLES>>2` = 140
output stereo pairs. So the engine mixes at ~45.75 kHz and downsamples to
32.03 kHz on the way out.

### Two effects are already off by default **[measured]**

- **Haas** (`Nas_Synth_Delay`) is gated on `AG.sound_mode == SOUND_OUTPUT_HEADSET`
  (`channel.c:45-53`). Default is STEREO ⇒ **off**.
- **Dolby surround** (`Nas_DolbySurround`) is gated on
  `AG.sound_mode == SOUND_OUTPUT_DOLBY_SURROUND` (`driver.c:1221`). Default
  STEREO ⇒ **off**.

Both are user-selectable in AC's options menu. On DC we lock the mode to
STEREO and delete both paths at zero cost relative to the default GC
experience. That removes two of the four "AC is heavier than SM64" items
for free.

Two more commands are **already no-ops in this port**: `A_CMD_UNK3`
(velocity convolution, GC microcode unknown) and `A_CMD_DISTFILTER`
(`rspsim.c:315, 395`). They cost nothing today and must not be "restored".

### No streamed audio **[measured]**

`internal/streamctrl.c` expects `/stream00.adp`…`/stream07.adp`. The GAFE01
FST contains **10 files, none of them `.adp`** (see `tools/dcasset/README.md`).
The disc-streaming subsystem is dead code in this game — **all** audio is
sequenced. There is no separate music-streaming port task.

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

| config | out / internal rate | voices | central | range |
|---|---|---|---|---|
| **A0** as-shipped | 32.03 k / 45.76 k | 24 | **68%** | 46–101% |
| A0 typical load (guess 12 voices) | 32.03 k / 45.76 k | 12 | 35% | 24–52% |
| **A1** half-rate | 22.05 k / 22.97 k | 24 | **34%** | 23–51% |
| **A2** half-rate, capped, FIR+comb off | 22.05 k / 22.97 k | 16 | **21%** | 14–32% |
| **A3** A2 + FP round-trip removed | 22.05 k / 22.97 k | 16 | 18% | 12–27% |
| **A4** budget-fitting | 22.05 k / 22.97 k | **10** | **13%** | 9–20% |

**The budget arithmetic that matters.** At 30 fps the frame is 33.3 ms. PLAN
§3.2's M3 gate allows game logic ≤25 ms. That leaves ≤8.3 ms for *everything
else* — renderer submission, T&L, texture work, disc I/O — so audio must land
at **≤4–6 ms/frame ≈ 12–18% CPU**. Only A4 (and A3 at ~10–12 voices) fits.
**Stage A at the game's real 24-voice cap does not fit the plan's own budget.**

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

## 3. Bank format and size — all **[measured]**

`audiorom.img` was extracted from the ISO at offset `0x000FE588`, 8,300,384
bytes, and parsed against `AudiodataHeaderStart` / `AudiobankHeaderStart` /
`AudiowaveHeaderStart` / `AudioseqHeaderStart` / `AudiomapHeaderStart` in
`src/static/jaudio_NES/game/audioheaders.c`.

### 3.1 Layout — three concatenated N64-style regions

`AudiodataHeaderStart` gives exactly three extents whose sizes **sum to the
file size to the byte**:

| region | offset | size | share |
|---|---|---:|---:|
| **Audioseq** (sequences) | 0x000000 | 849,664 | 10.2% |
| **Audiobank** (soundfont metadata) | 0x0CF700 | 425,088 | 5.1% |
| **Audiotable** (sample data) | 0x137380 | **7,025,632** | **84.6%** |

There is no container magic, no compression, no JAudio/IBNK/WSYS/AAF chunk
anywhere in the file (scanned; zero hits). It is a raw N64 audio ROM image.

### 3.2 Sequences and soundfonts

- **249 sequences.** median 2,176 B, mean 3,412 B, max 120,192 B.
  243 `CACHE_LOAD_TEMPORARY`, 4 persistent, 2 permanent.
- **159 soundfont entries** (154 non-empty). Metadata median 2,144 B, max
  20,736 B. Standard Zelda layout: `{Drum** drums; SoundEffect* sfx;
  Instrument* instruments[nInst];}`, with counts packed into the `ArcEntry`
  params exactly as `__SetVlute` reads them (`param0` = waveBankId0<<8|Id1,
  `param1` = nInst<<8|nDrums, `param2` = nSfx).
- **6 sample banks** (Audiotable entries): 239,936 / 57,088 / 434,304 /
  1,189,600 / 986,656 / **4,118,048** bytes.
- **Sequence→soundfont map** (`AudiomapHeaderStart`, 504 u16 = 1008 B):
  **247 of 249 sequences reference exactly one soundfont.** One references 2,
  one references 4.

### 3.3 Sample encoding

**All 1157 unique samples are `CODEC_ADPCM` = N64 VADPCM**, `medium = RAM`,
`order = 2`, `n_predictors = 2` (64-byte codebook each), 9 bytes per 16-sample
frame (4.5 bits/sample), 16-bit output. No S8, no S16, no SMALL_ADPCM, no
CODEC_REVERB in the shipped data — the multi-codec branches in
`Nas_SynthMain` are dead weight on this title.

- referenced sample bytes: **6,734,836** of the 7,025,632 Audiotable (95.9%;
  the rest is alignment/unreferenced)
- decoded PCM16 sample count: **≈ 11.97 M samples** (≈ 24 MB as PCM16)
- decoded length: median 5,312 samples, p90 21,040, **max 225,280**
- loop field: 451 samples loop forever (`count = 0xFFFFFFFF`), 706 are
  one-shot (`count = 0`). **No sample uses `count == 2`**, so the
  `stop_loop`/`sample_end` branch in `Nas_SynthMain` is dead.
- tuning floats: min 0.036, median 0.689, max 8.24 (drives pitch/decode ratio)

### 3.4 Converted to AICA 4-bit Yamaha ADPCM

4 bits/sample vs VADPCM's 4.5 ⇒ **× 8/9 = 0.889** of the source bytes.

| set | VADPCM | AICA 4-bit |
|---|---:|---:|
| **everything** | 6,734,836 | **5,986,040 (5.71 MB)** — does *not* fit 2 MB |
| wave bank 5 alone | 4,113,838 | 3,656,745 (3.49 MB) |
| **largest single soundfont** (bank 153, 126 instruments) | 2,217,394 | **1,971,016 (1.88 MB)** |
| **largest single sequence's set** (seq 242, banks {2,155,154,153}) | 3,844,022 | **3,416,908 (3.26 MB)** |
| second-largest sequence (seq 247, bank 153) | 2,217,394 | 1,971,016 (1.88 MB) |
| **median sequence** | 56,960 | **≈ 49 KB** |
| mean sequence | 83,601 | ≈ 72 KB |

**This is the decisive finding.** The 8.3 MB headline number is irrelevant.
The *resident* working set is per-sequence, and **the median sequence needs
49 KB of AICA sound RAM**. Only two sequences (242 and 247, the big
multi-bank music cues) approach or exceed the ~1.8 MB usable sound RAM.
No individual soundfont exceeds 2 MB.

**⇒ Answer to "convert offline or stream from disc": convert offline, page
per-sequence into sound RAM, and handle the two outliers specially.** No
per-sample disc streaming is needed for the common case.

### 3.5 The AICA constraints that bite

1. **65,536-sample per-channel limit.** 24 samples decode to more than 65,536
   samples (total 1,438,238 VADPCM bytes, mostly wave bank 5). These must be
   split across chained channels, converted to a CPU-fed ring, or
   downsampled below the limit. **[measured]**
2. **AICA ADPCM is stateful (Yamaha step-index)**, unlike VADPCM which resets
   its predictor state per 16-sample frame *and* carries an explicit
   `predictor_state[16]` at the loop point. A loop that jumps back into the
   middle of an AICA ADPCM stream produces a step-index discontinuity → audible
   click or slow drift. 451 samples loop. Mitigations, in order: (a) encode
   loop-carrying samples so the encoder state converges at `loop_end` ≈
   `loop_start` (dcaconv/wav2adpcm territory), (b) store looping samples as
   8-bit PCM (2× the bytes — still fits given §3.4), (c) rotate the sample so
   the loop starts at a block boundary. **This is the main stage-B risk.**
3. **~1.8 MB usable** of the 2 MB after the KOS ARM driver + `snd_stream`
   buffers. *(Reserve figure is an estimate — verify at M0.)*

---

## 4. Stage A — rspsim on SH-4, reduced rate, feeding `snd_stream`

Purpose: **bring-up and oracle**, not the shipping configuration. It gets
correct, sequenced, in-tune audio running with zero format work, and it is the
reference the stage-B backend gets A/B'd against.

### 4.1 Configuration changes

| knob | from | to | effect |
|---|---|---|---|
| `NA_SPEC_CONFIG[0]._00` | 0xBB80 (48000) | **0x5DC0 (24000)** | `num_samples_per_update` 200 → 100; halves *all* per-voice work |
| `JAC_FRAMESAMPLES` | 560 | **384** (must be ×4) | output pairs/frame |
| `JAC_DAC_RATE` | 32028.5 | **22050.0** | audio frame rate becomes 57.42 Hz |
| `DAC_SIZE` | 1120 | **768** | = 2 × FRAMESAMPLES |
| `AG.sound_mode` | STEREO (runtime-settable) | **locked STEREO** | deletes Haas + Dolby |
| `AG.num_channels` (`spec->_05`) | 24 | **compile-time cap, default 12** | the actual budget lever |

`updates_per_frame` stays 4 — dropping to 2 would double envelope-ramp
granularity error (`Nas_Synth_Envelope` divides by `samples_per_update >> 3`)
and is not worth it.

### 4.2 Effect tiering — cut order, cost, and what it sounds like

Ordered by **cycles saved per unit of perceptual loss**, cheapest loss first:

1. **Haas + Dolby** — free (already off in STEREO). Delete `Nas_Synth_Delay`,
   `Nas_DolbySurround`, `A_CMD_RESAMPLE_ZOH`, the headset pan tables.
   *Perceptual: none vs. the default GC experience.*
2. **FIR filter** (`common->filter`, `LSF_TABLE`/`HSF_TABLE`) — saves ~27% on
   any voice that uses it. *Perceptual: those instruments lose their low/high
   shelf and sound brighter and harsher — noticeable on soft/muted patches,
   not on percussion.* Cut second because it is the biggest single per-voice
   win.
3. **Comb filter** — saves ~10% on voices that use it. *Perceptual: loses the
   short-delay thickening/chorus on the handful of patches that set it; those
   patches sound thinner and more "dry sample".*
4. **Reverb** — saves only ~1.7% of the CPU but costs the interiors, the
   museum, and the shop their sense of space. **Keep it.** This is the
   counter-intuitive result: the global reverb is by far the best
   cycles-per-perceived-quality in the engine, and the per-voice effects are
   the expensive ones.
5. **Output rate** — 22.05 kHz vs 32 kHz. *Perceptual: audible loss of air on
   cymbals/bells and slightly grainier ADPCM noise; AC's music is mostly
   mid-band and survives well. This is the single largest CPU lever and should
   be spent before cutting voices.*
6. **Voice cap** 24 → 12. *Perceptual: the real cost. Dense music cues will
   drop notes via the existing priority-based voice stealing
   (`channel.c`/`oneshot.c`); SFX-heavy moments will duck music notes.
   Do this last and make it a settings-menu slider.*

Free wins that are not cuts:
- **Delete the `f32` round-trip in `A_CMD_ENVMIXER`** (`rspsim.c:502-505`) and
  in the `var_r3_3 = var_r20[0] + var_f2` adds. The values are integers; the
  float is a decomp matching artifact. ~20 cycles/voice-sample ≈ **16%**.
  Guard it behind a compile-time flag and A/B against the PC build's output.
- Delete the dead codec branches (only `CODEC_ADPCM` exists) and the
  `loopInfo->count == 2` branch.
- `A_CMD_ADPCM`/`A_CMD_FIRFILTER` inner MAC loops are the obvious candidates
  for hand-written SH-4 `MAC.W` asm.

### 4.3 Output plumbing

Replace `pc_audio.c`'s SDL device with KOS:
- `snd_stream_alloc(cb, SND_STREAM_BUFFER_MAX)`, 22050 Hz, stereo, 16-bit.
- Keep the existing SPSC ring (`pc_audio.c:25-105`) — it is already
  lock-free and correct; swap `SDL_atomic_t` for KOS atomics/volatile.
- `AIInitDMA(addr,size)` stays the producer-side entry point verbatim.
- Run the producer on a **lower-priority KOS thread** with a hard budget, and
  poll `snd_stream_poll()` from it. Buffer ≥4 audio frames (~70 ms) — the same
  threshold the PC build uses (`AUDIO_PRODUCE_THRESHOLD 4480`), scaled.
- Underrun handling must **drop** an audio frame, never block the game thread.

### 4.4 Expected cost

**13% CPU (range 9–20%) at 10 voices / 22.05 kHz / reverb on / FIR+comb off**
= ~4.3 ms of a 33.3 ms frame. At the full 24-voice cap it is 34% (23–51%) and
does not fit.

### 4.5 Main-RAM footprint (stage A) **[measured sizes, summed]**

`audiorom.img` never becomes resident; only the engine's own heaps do:
`AGC.acmdBufSize` 0x70000 (458,752 B, the heap handed to `Nas_InitAudio`),
`fixSize` 0x38000 (229,376), `ememSize` 0x28000 (163,840), session cache heaps
0xE700 (59,136), 4 KB DMEM, 4 × 0xF80 AI buffers (15,872), 8 KB reverb rings,
72 waveload buffers × 0x400 (73,728), 2 × 2240 B DSP bufs.
**≈ 0.55–0.6 MB of main RAM** — must appear as a line item in `kb/mem-budget.md`.
These are GC-era sizings and can be cut hard (the acmd buffers alone only need
2350 × 8 B).

---

## 5. Stage B — AICA hardware voices

Sequencing stays on the SH-4 (cheap: `Nas_MySeqMain` is script interpretation
at 4 × 57 Hz over ≤16 subtracks × 5 groups). Synthesis moves to the 64
hardware ADPCM channels.

### 5.1 The exact seam

Keep, unchanged:
- `Nas_smzAudioFrame` (`driver.c:171`) — **first loop only**:
  `Nas_MySeqMain(i-1)` + `__Nas_PushDrvReg(...)`. This is the sequencer plus
  the per-update snapshot of every voice into
  `AG.common_channel[updateIdx * AG.num_channels + i]`.
- All of `track.c`, `jammain_2.c`, `channel.c`, `oneshot.c`, `sub_sys.c`,
  `effect.c`, `system.c` (bank/sequence loading), `memory.c` (heaps).

Replace, wholesale:
- `Nas_DriveRsp` (`driver.c:541`) → `dc_aica_frame(updateIndex)`
- `Nas_SynthMain` (`driver.c:659`) → `dc_aica_voice_update(chan_id, common, driver)`
- `Nas_Synth_Resample` (1287), `Nas_Synth_Envelope` (1299),
  `Nas_Synth_Delay` (1399), `Nas_DolbySurround` (1246) → deleted
- `Nas_CpuFX` (`driver.c:45`), all `Nas_*AuxBuffer*` reverb plumbing → deleted
  or replaced by an AICA DSP send
- `RspStart` / `RspStart2` (`rspsim.c`) → deleted (keep behind
  `DC_AUDIO_SOFTWARE=1` as the stage-A oracle)
- `Nas_WaveDmaCallBack` (`system.c:326`) → `dc_aica_sample_resident(sample_id)`
  — the waveload cache becomes the sound-RAM residency manager
- `MixCpu`/`AIInitDMA` output path → unused (AICA mixes)

Every value the backend needs is already in `commonch`
(`include/jaudio_NES/audiostruct.h:242`) and `driverch` (:292):

| AICA register | source field |
|---|---|
| sample start / loop start / loop end (SA, LSA, LEA) | `common->tuned_sample->wavetable->{sample,loop}` → converted-bank sample id |
| loop enable (LPCTL) | `loop->count != 0` (only 0 or 0xFFFFFFFF occur) |
| pitch (OCT/FNS) | `common->frequency_fixed_point` (16.16 ratio vs mixing rate) |
| volume + pan (DISDL/DIPAN) | `common->target_volume_left/right`, ramped by `driver->current_volume_left/right` |
| DSP send level (DSP channel) | `common->target_reverb_volume` / `driver->cur_reverb_vol` |
| key-on / key-off | `common->needs_init` / `common->enabled` / `common->finished` |

**Envelope: keep it on the SH-4.** AICA's hardware EG is a 4-stage
AR/D1R/D2R/RR shape; jaudio envelopes are arbitrary point lists (`envdat`) with
`ADSR_HANG`/`ADSR_GOTO`/`ADSR_RESTART` opcodes that will not map. Writing the
channel volume register once per update (4 × 57 Hz = 229 Hz) is a handful of
stores per voice and reproduces the exact GC envelope. This is the design
decision that makes stage B behaviourally faithful.

### 5.2 Voice budget

24 engine voices vs 64 AICA channels — comfortable, with headroom for
split-sample chaining (§3.5) and a spare pair for the two >2 MB sequences.
Haas would have needed a second delayed channel per voice; it is off.

### 5.3 What the offline converter must produce

1. **Per-sample AICA asset**: VADPCM (order 2, 2 predictors) → PCM16 → AICA
   4-bit ADPCM (default) or 8-bit PCM (for loop-critical samples, §3.5.2),
   32-byte aligned, with `loop_start`/`loop_end` remapped from VADPCM
   sample-frame indices to AICA sample indices.
2. **Sample manifest**: `{id, format, sound_ram_bytes, n_samples, loop_start,
   loop_end, loop_flag, base_pitch (from the `wtstr.tuning` float),
   src_wave_bank, src_offset, split_of/split_index}`.
3. **Rewritten bank metadata**: the 154 soundfonts with `smzwavetable*`
   pointers replaced by sample ids; envelopes, key regions, tuning, ADSR
   indices preserved byte-for-byte. Total 425 KB today; will shrink.
4. **Residency sets**: per-soundfont and per-sequence sample-id lists
   (derivable from §3.2's map — the tool must emit them so the loader can DMA
   exactly the right bytes and nothing else).
5. **Split plan** for the 24 over-length samples.
6. **A validation report**: round-trip SNR per sample (VADPCM→PCM16→AICA→PCM16),
   worst offenders listed, and loop-seam discontinuity in LSBs.

### 5.4 Residency policy

- Budget ~1.6 MB of the ~1.8 MB usable sound RAM for samples; 200 KB for
  streaming/scratch.
- Load per **sequence start** (the map already gives the exact set). Median
  49 KB ⇒ ~0.1 s at CD-R's 500 KB/s. Prefetch on the sequence-preload path
  that already exists (`Nas_PreLoadSeq`/`Nas_PreLoadBank`, `system.c:595`).
- Concurrent sequences across the 5 groups (BGM + SFX + voice) sum their sets;
  keep an LRU over sample ids, not over banks.
- **Sequence 242 (3.26 MB) and 247 (1.88 MB) are the two exceptions.** Options:
  downsample their bank-153 samples to 22 kHz before conversion (halves them),
  8→4-bit where safe, or split the cue. Decide with ears at M4.

---

## 6. `tools/` — what must be built offline

| tool | input | output |
|---|---|---|
| `tools/audioextract` | ISO (via existing `tools/dcasset` / `pc_disc.c`) | `audioseq.bin`, `audiobank.bin`, `audiotable.bin` split at the three `AudiodataHeaderStart` extents (0x0 / 0xCF700 / 0x137380); per-entry blobs + JSON index for all 249 seq / 159 bank / 6 wave entries |
| `tools/vadpcm` | Audiotable + codebooks from Audiobank | reference VADPCM→PCM16 decoder (order 2 / 2 predictors), shared by the converter and the validator |
| `tools/bankconv` | Audiobank + Audiotable | AICA sample assets + manifest + rewritten bank metadata (§5.3 items 1–3, 5) |
| `tools/aicapack` | manifest + residency sets | per-sequence sound-RAM images, 32 B aligned, ≤1.6 MB each, with the split plan applied |
| `tools/audiomap` | `AudiomapHeaderStart` + bank metadata | per-sequence and per-soundfont sample-id residency sets (§5.3 item 4) |
| `tools/audiocheck` | everything above | round-trip SNR report, loop-seam report, over-2 MB sequence report, over-65536-sample report (§5.3 item 6) |

Notes:
- The header tables live in **C source** (`game/audioheaders.c`), not in the
  ISO — the tools must parse that file (or a generated JSON dump of it) to get
  the extents, cache types and `param0/1/2` counts. A ~60-line parser is
  sufficient; one was written and validated during this pass.
- Nothing here needs the game running. All of it is host-side Python/C.
- Reuse `tools/dcasset`'s ISO reader; do not add a second one.

---

## 7. What is NOT measured, and how to measure it

These are the numbers that would collapse the error bars. Ranked by value.

1. **Actual live voice count.** Instrument `Nas_DriveRsp` (`driver.c:541`):
   log `noteCount` and its max per second, on the existing PC build, across
   town / shop / museum / K.K. Saturday / a fishing tournament. This single
   number moves the CPU estimate by 2×. *Cheap — do it first, on the PC build,
   before any DC work.*
2. **Fraction of voices using `filter` / `comb_filter_size`.** Same harness,
   same run — count `common->filter != 0` and `comb_filter_size != 0` per
   frame. Decides whether cutting them is worth the perceptual loss.
3. **Real SH-4 cycles.** The 80–180 cyc/voice-sample band collapses to a
   measurement the moment `dc/build-dc-docker.sh` exists: build `rspsim.c`
   standalone for sh-elf, run a fixed 1000-frame command list under Flycast
   with the SH-4 cycle counter (`PMCR`/`TMU`), and report cycles per
   voice-sample. **This is the single most valuable M1 measurement for audio.**
4. **AICA sound-RAM reserve** actually consumed by the KOS ARM driver +
   `snd_stream` buffers (the ~200 KB figure in §3.5 is an estimate).
5. **AICA ADPCM loop-seam audibility** — convert the 451 looping samples,
   listen, and count how many need the 8-bit PCM fallback.
6. Whether the ~4.7% tempo error (57.19 Hz audio frames vs the engine's
   assumed 60 Hz `refresh_rate`) is inherited from GC or introduced by the PC
   port. It is not a new DC problem either way.

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

---

## 9. Verdict and recommended plan of record

**Audio is a real risk.** It is not the fatal one — RAM and game-logic speed
still rank above it — but PLAN §3.4's framing ("Stage B *if* A blows the CPU
budget") should be inverted:

1. **Build stage A at M3** as specified in §4: 22.05 kHz, STEREO locked,
   FP round-trip removed, voice cap exposed as a setting, default 12.
   Expect **13% CPU (9–20%)** at 10–12 voices. It is the fastest path to
   correct sequenced audio and it is the byte-exact oracle for stage B.
2. **Assume stage B is required** and start the offline converter
   (`tools/bankconv`) at M1, in parallel — it is pure host-side work with no
   dependency on the DC toolchain, and §3's measurements say it will work:
   median sequence 49 KB in sound RAM, no soundfont over 2 MB, a single
   uniform codec to convert.
3. **Take measurement #1 in §7 (live voice count) on the PC build this week.**
   If AC really only runs 8–12 voices in a town, stage A at 22 kHz fits the
   budget and stage B becomes a quality/headroom upgrade rather than a
   requirement. If it runs 20+, stage B is mandatory and should be scheduled
   into M4 explicitly rather than listed as a contingency.

The things that make this tractable and were not known before this pass: the
bank is a **single uniform codec** (VADPCM order 2 / 2 predictors, 1157
samples), the residency unit is the **sequence** and its median size is
**49 KB**, and the two most expensive optional effects are **already disabled**
in the game's default sound mode.
