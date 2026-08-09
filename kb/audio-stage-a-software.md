# Audio stage A — rspsim on SH-4 at reduced rate, feeding `snd_stream`

§4 of `kb/audio-plan.md`, moved verbatim: the config changes, the effect-cut
order with perceptual notes, the KOS output plumbing, expected cost and the
stage-A main-RAM footprint. Read when implementing bring-up audio.
~~**Stage A is the bring-up path and the oracle, not the shipping configuration —
at the game's real 24-voice cap it does not fit the budget**~~
🛑 **[STALE 2026-08-06 — see §0.]** Stage A is still the bring-up path and the
oracle; whether it can also be the *shipping* configuration is an open question
again (`kb/audio-plan.md`).

---

## 🛑 0. READ FIRST — the premise of "stage A does not fit" is void (2026-08-06)

### 0.1 `src/` is not `-O0` any more

The `-O0` directive on `src/` was reversed on 2026-08-06. `src/` builds at
`-Os` with a 18-TU `-O3` hot list (`DC_OPT_PROFILE=perf`, the default);
`DC_OPT_PROFILE=size` is `-Os` everywhere; `DC_OPT_PROFILE=o0` is a
byte-identical revert. **Everything under `src/static/jaudio_NES/` —
`internal/rspsim.c`, `driver.c`, `internal/jammain_2.c` — is `-Os` like the
rest of the tree.**

So the argument this document has carried since 2026-08-01 — *stage A costs
N % of the SH-4, N is fixed because codegen is fixed, therefore stage A cannot
ship* — **has lost its middle term.** §4.4's "13 % CPU (range 9–20 %) … at the
full 24-voice cap it is 34 % and does not fit" is an `-O0` number and is stale.

**Do not scale it.** No conversion factor is offered here, and none may be
invented. The only measured proxy is the frameskipped game-logic tick — all of
`game_main` with the draw skipped, i.e. general `src/` code, which `jaudio_NES`
also is — which went **6.6 ms → 2.8 ms, a 58 % fall**. That is a proxy for the
*class* of code, not a measurement of the mixer.

**The re-measurement is one line:** an audio-on town smoke at
`DC_OPT_PROFILE=perf` versus the same tree at `DC_OPT_PROFILE=o0`. Both
profiles exist; a full rebuild is 96 s. **Until that pair exists, the CPU cost
of software synthesis is an open question, not a fixed input, and the
stage-A-vs-stage-B decision must not be taken.**

Evidence: `kb/state-log.md`, entry **2026-08-06**.

### 0.2 ⚠️ `jammain_2.c` is the first file to suspect the day audio starts

The 2026-08-06 warnscan (`DC_TARGET=warnscan bash dc/build-dc.sh`, all 3,926
TUs at `-O2` with the decomp's `-w` removed, reduced by
`tools/dcopt/warnscan_report.py`) named **`jammain_2.c` the single riskiest file
in the tree**: it is compiled as **C++**, it has a **missing return**, and it has
**22 uninitialised reads — the most of any file in the tree**. In C++ a missing
return is UB that G++ turns into `__builtin_unreachable` and deletes the path
outright, which is exactly the shape of a boot-time wild-pointer crash.

It is **not quarantined**, and the only reason it is not is that
`DC_AUDIO=0` means it never ticks. **The day audio work starts, that changes.**
If the audio-on `perf` build misbehaves and the `o0` build does not, the tools,
in order:

1. `DC_OPT_O0_EXTRA=src/static/jaudio_NES/internal/jammain_2.c` — quarantine it
   alone.
2. `DC_AUTOVAR_INIT=zero` — if the symptom vanishes, the bug is one of the 22
   uninitialised reads, not the optimizer.
3. `tools/dcopt/bisect_o0.sh` — binary-search the rest of the audio TUs.


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

Ordered by **cycles saved per unit of perceptual loss**, cheapest loss first.

⚠️ **[2026-08-06] The ORDER survives; the URGENCY does not.** Every "saves ~N %"
below is a fraction of an `-O0` cost (§0.1), and the ladder was written to be
descended until stage A fit a budget it could not reach. It may now not need
descending as far — cut 1 is free, and cuts 5 and 6 (output rate, then voice
cap) are the ones that cost the player something. **Measure first (§0.1), then
decide how far down this list to go.** Do not pre-commit to the 22 kHz decision
or to a 12-voice cap on the strength of the old numbers.


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

### 4.4 Expected cost — 🛑 **STALE 2026-08-06, see §0.1**

~~**13% CPU (range 9–20%) at 10 voices / 22.05 kHz / reverb on / FIR+comb off**
= ~4.3 ms of a 33.3 ms frame. At the full 24-voice cap it is 34% (23–51%) and
does not fit.~~

Both figures are modelled on `-O0` codegen and the "does not fit" conclusion
falls with them. **There is no replacement number in this document and none may
be derived here** — run the `perf`-vs-`o0` audio-on A/B in §0.1 and write the
measured figure in. Until then the honest statement is: *the cost of stage A on
the current build is unmeasured.*

### 4.5 Main-RAM footprint (stage A) **[measured sizes, summed]**

`audiorom.img` never becomes resident; only the engine's own heaps do:
`AGC.acmdBufSize` 0x70000 (458,752 B, the heap handed to `Nas_InitAudio`),
`fixSize` 0x38000 (229,376), `ememSize` 0x28000 (163,840), session cache heaps
0xE700 (59,136), 4 KB DMEM, 4 × 0xF80 AI buffers (15,872), 8 KB reverb rings,
72 waveload buffers × 0x400 (73,728), 2 × 2240 B DSP bufs.
**≈ 0.55–0.6 MB of main RAM** — must appear as a line item in `kb/mem-budget-m1-sh4.md`.
These are GC-era sizings and can be cut hard (the acmd buffers alone only need
2350 × 8 B).
