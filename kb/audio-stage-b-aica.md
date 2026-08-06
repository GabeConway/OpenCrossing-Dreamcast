# Audio stage B — AICA hardware voices, and the offline converter

§5 and §6 of `kb/audio-plan.md`, moved verbatim: the exact seam in the engine,
the register mapping, the residency policy, and the `tools/` that must be built
host-side. Read when starting the bank converter or the AICA backend.
~~**Plan on stage B being required, not a fallback**~~ 🛑 **[STALE 2026-08-06 —
see §0.]** Stage B is a fallback again until re-measured; audio is still a real
risk, not a solved problem (`kb/audio-plan.md`).

---

## 🛑 0. "STAGE B IS REQUIRED" HAS LOST ITS EVIDENCE (2026-08-06)

Stage B was never chosen on its merits. It was chosen because the software
mixer's SH-4 cost was believed immovable: `src/` was frozen at `-O0` by
directive, the modelled cost of stage A exceeded the frame budget at every
voice count, and therefore the work had to move to hardware voices.

**That directive was reversed on 2026-08-06.** `src/` now builds at `-Os` with
a 14-TU `-O3` hot list (`DC_OPT_PROFILE=perf`, the default), with
`DC_OPT_PROFILE=o0` retained as a byte-identical revert. `jaudio_NES` is `-Os`.
**The premise is void, and with it the conclusion that stage B is required.**
The CPU cost of software synthesis is now an **open question**, not a fixed
input — and it moved in software's favour by an amount nobody has measured.

**Do not scale the old audio numbers, here or anywhere else** — no honest
factor exists. The only measured proxy is the frameskipped game-logic tick
(all of `game_main`, draw skipped, general `src/` code — which `jaudio_NES` is
too): **6.6 ms → 2.8 ms, a 58 % fall.** A proxy is not a result.

**The decision gate:** an audio-on town smoke at `DC_OPT_PROFILE=perf` versus
the same tree at `DC_OPT_PROFILE=o0`. Both profiles exist and a full rebuild is
96 s, so this A/B is one line and it is cheap next to the tool chain in §6.
**Do not take the stage-A-vs-stage-B decision until it has run.**

### What this does and does not change about the work below

- **Unaffected — the offline host-side tools (§6) and the bank format work.**
  They are the same work whichever stage ships: `tools/vadpcm` and
  `tools/audiocheck` are needed to validate *any* audio path, and stage A wants
  the same extraction. Building them is still the right parallel task; what has
  changed is that they are no longer racing a deadline.
- **Unaffected — the sound-RAM findings.** Bank 153 missing usable AICA RAM by
  70,472 B, the 1,900,544 B usable figure, the residency map: those are facts
  about the hardware and the data, not about codegen.
- **Reopened — everything scheduling-shaped.** Whether stage B is on the
  critical path, whether `tools/bankconv` and `tools/aicapack` (§5.3, §6) are
  needed at all, and whether the engine seam in §5.1 ever gets cut.
- **Unaffected — the seam design itself (§5.1) is still correct if stage B is
  built.** Nothing here needs redesigning; it may just not need doing.

Evidence: `kb/state-log.md`, entry **2026-08-06**.

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
