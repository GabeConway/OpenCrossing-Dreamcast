# The AICA offload (stage B) — what is MEASURED, and what the measurement changed

**Written 2026-08-13.** Supersedes the sizing arithmetic in
`kb/audio-engine.md` §3.5 and `kb/audio-stage-b-aica.md` §5.4 — not because
that arithmetic was wrong (it was right to within rounding) but because it was
arithmetic, and this is the bank.

**Why this work is ranked first at all:** the hardware gprof of 2026-08-12 put
`RspStart` at **14.1 ms of a 71.9 ms frame** and audio as a family at **17.2 ms
(23.9 % of busy CPU)**, at **2.13×** its Flycast share. It is the only
subsystem that measurably gets *worse* on silicon. `kb/RESUME.md` §6c.

The host tool is `tools/dcaudio/`. Every number below is reproducible with:

```bash
python3 tools/dcaudio/census.py
python3 tools/dcaudio/tests/test_vadpcm.py
```

---

## 1. The decoder is bit-exact against the shipped C, and that is checked

`tools/dcaudio/vadpcm.py` is a transliteration of `A_CMD_ADPCM`
(`rspsim.c:107`, the `TARGET_PC` arm — the one this port compiles), not a
reimplementation from a spec. `tools/dcaudio/tests/ref_adpcm.c` lifts that
frame loop verbatim into a host binary and
`tools/dcaudio/tests/test_vadpcm.py` drives both with random frames:

```
200/200 trials bit-exact (76,800 samples compared)
```

Coverage spans the full 0..15 scale range and s16-wide coefficients, so both
`_clamp_s16` arms and the s32 wrap are exercised. Two traps that a
from-the-spec decoder gets wrong:

- **The `TARGET_PC` arm accumulates at full width and applies ONE arithmetic
  `>> 11`.** The GameCube arm divides each term by `0x800` separately, which
  truncates toward zero per term and gives different samples. rspsim.c:151-153.
- **`sp9C` is s16 but `sp7C`/`sp5C` are s32** (rspsim.c:85-87), so the shifted
  nibbles do not wrap where the nibbles did. The oracle must be built
  `-fwrapv` or the test measures the host compiler instead of the console.

---

## 2. `audiorom.img` has no index in it — the index is compiled into the game

This is the fact that makes a host tool possible at all, and it is not written
down anywhere else. All five tables are static C arrays in
**`src/static/jaudio_NES/game/audioheaders.c`**:

| symbol | entries | role |
|---|---|---|
| `AudiodataHeaderStart` | 3 | the three extents |
| `AudioseqHeaderStart` | 249 | sequences @ `0x000000` |
| `AudiobankHeaderStart` | 159 | soundfonts (ctl) @ `0x0CF700` |
| `AudiowaveHeaderStart` | 6 | sample banks (tbl) @ `0x137380` |
| `AudiomapHeaderStart` | 504 × u16 | sequence → soundfont |

`Nas_BankHeaderInit` (`system.c:565-576`) adds the extent base to each
`ArcEntry.addr` at boot; `AudiodataHeaderStart` is never rebased, so its addrs
are raw file offsets. The three extents sum to 8,300,384 — the exact file size.

⭐ **THE JOIN KEY BETWEEN HOST AND RUNTIME.** `Nas_StartDma` computes
`aram_offset = device_addr + GetNeosRomTop()` (`system.c:1299`), and
`GetNeosRomTop()` is a pure ARAM base carrying no file-offset component
(`dummyrom.c:25-27`). **So a sample's `device_addr` IS its raw byte offset into
`audiorom.img`** — stable across builds, computable offline, recoverable at
runtime as `sampleAddr - GetNeosRomTop()`. That is the manifest key.

### Four traps that silently produce a wrong answer

- **Alias entries.** `size == 0` means `addr` is the *index of another entry*,
  not an offset (`__Link_BankNum`, `system.c:1023-1031`). 5 sequences and 5
  banks are aliases. Reading an alias as an offset lands near the start of the
  extent and *parses*.
- **A ctl blob does NOT begin with `voiceinfo`.** `voiceinfo`
  (`audiostruct.h:396`) is RAM-only, built from the ArcEntry's param words by
  `__SetVlute` (`system.c:1488-1497`). Offset 0 is a u32 offset table:
  `[0]` percussion, `[1]` sfx, `[2+i]` instrument i.
- **`medium` in an UNRELOCATED `smzwavetable` is a wave-bank selector, not a
  storage medium.** 0 → `wave_bank_id0`, 1 → `wave_bank_id1`. `__WaveTouch`
  overwrites the field with the real medium at `system.c:2123`/`:2128`.
- **Three different bases in one 16-byte struct**: `sample` is
  WAVE-BANK-relative, `loop` and `book` are CTL-BLOB-relative
  (`system.c:2113-2121`).

---

## 3. The census — measured, and it confirms the kb's arithmetic

```
unique samples   1157        VADPCM on disc   6,734,836 B
looping 451   one-shot 706   decoded          11,972,323 samples
as AICA 4-bit    5,986,283 B
```

| figure | kb said | measured | verdict |
|---|---|---:|---|
| everything as AICA 4-bit | 5,986,040 | **5,986,283** | ✅ |
| bank 153 | 1,971,016 | **1,971,002** | ✅ |
| seq 242 | 3,416,908 | **3,416,862** | ✅ |
| median sequence | "≈49 KB" | **50,628 B** | ✅ |
| sequences using one soundfont | 247/249 | **247/249** | ✅ |
| samples over the channel limit | 24 | **24** | ✅ |
| longest sample | 225,280 | **225,280** | ✅ |

**The ×8/9 arithmetic was sound.** Differences are rounding — the kb scaled
byte counts, this scales exact sample counts.

---

## 4. ⭐ WHAT IS NEW: two of the four blockers are ONE blocker

`kb/audio-engine.md` §3.5 lists B1 (24 samples exceed a hardware channel) and
B3 (bank 153 is 70,472 B too big for sound RAM) as separate problems. **They
are the same problem.**

```
bank 153                          1,971,002 B   126 samples   103.7 % of usable
  its 19 over-limit samples       1,030,632 B
bank 153 without them               940,370 B    49.5 % of usable   <- FITS
```

**19 of bank 153's 126 samples are over the channel limit and are 52 % of its
bytes.** All 24 over-limit samples in the whole game live in wave bank 5, and
they are **1,278,432 B of the 5,986,283 B total — 21 % of everything, in 24
samples (2 % of the count).**

⭐ **CONSEQUENCE: solve the length problem and the fit problem dissolves with
it.** Those 24 are long-form material (up to 225,280 samples ≈ 10 s at 22 kHz);
they are exactly what should be *streamed* rather than made resident. The
natural architecture is a length split: everything ≤ 65,534 samples is
resident, the 24 long ones get a streaming path — which is what `snd_stream`
already does, and what AICA's `AICA_SM_ADPCM_LS` mode exists for.

---

## 5. 🔴 A kb MITIGATION IS FALSIFIED: 8-bit PCM for looping samples does not fit

`kb/audio-engine.md` §3.5's mitigation (b) for the loop-discontinuity blocker
is *"store looping samples as 8-bit PCM — 2× the bytes, still fits given §3.4"*.

```
everything, all 4-bit                        5,986,283 B
...with all 451 looping samples at 8-bit     9,310,243 B   = 4.9x usable
```

**It does not still fit.** As a blanket policy it is 4.9× the 1,900,544 B of
usable sound RAM, against 3.1× for the all-4-bit case — it makes the residency
problem substantially *worse*, not marginally. It may survive as a targeted
fallback for a handful of named samples; it is dead as a policy.
`kb/audio-plan-of-record.md` §7 item 5 should be read with this in hand.

---

## 6. ⭐ The loop-discontinuity blocker, MEASURED for the first time

`kb/audio-plan-of-record.md` §7 open item 5 asks: *"convert the 451 looping
samples, listen, and count."* Listening still decides, but the search is now
bounded. `aica_adpcm.analyse_loop()` encodes each sample, walks the decoder to
`loop_start` and again to `loop_end`, and compares the two states — because a
sample whose state already matches at both ends **cannot click regardless of
what the hardware does at the loop point**, both hypotheses giving the same
decoder.

All 451 looping samples, predictor delta across the loop:

| `history` delta | count | reading |
|---|---:|---|
| 0 (exact) | 3 | immune |
| < −60 dBFS | 89 | inaudible |
| −60..−40 dBFS | 196 | inaudible to marginal |
| −40..−20 dBFS | 151 | potentially audible |
| > −20 dBFS | 12 | audible |

🔴 **BUT THE WHOLE TABLE IS CONDITIONAL, AND THE CONDITION IS UNRESOLVED.** It
only matters **if AICA resets decoder state at the loop point**. If state is
carried across the loop, the discontinuity does not exist and **zero** samples
are at risk. So the blocker's real size is:

- **hardware carries state → 0 samples at risk.**
- **hardware resets state → ~163 samples at risk** (151 + 12), of which 12 are
  unambiguous.

**That single hardware fact is worth one burn** and it collapses a blocker the
kb has carried as "the main stage-B risk" since it was written.

### What is known about the condition

Not answerable from KOS source — the ARM driver sets LSA/LEA and walks away.
The circumstantial evidence points to *carried*, for mode 3 specifically:

- The mode field (reg 0 bits 8-7) has four values, including
  **`AICA_SM_ADPCM = 2`** and **`AICA_SM_ADPCM_LS = 3`** ("long stream")
  (`aica_comm.h:156-159`).
- KOS uses **mode 2 for one-shot sound effects** (`snd_sfxmgr.c:277`).
- KOS uses **mode 3 exclusively for its looping ring-buffer streamer**
  (`snd_stream.c:602-603`), over a double buffer whose halves are refilled in
  time order. **That can only work if state is carried across the wrap** — a
  reset would inject a discontinuity every buffer period.

⚠️ **INFERENCE, NOT A DOCUMENTED HARDWARE STATEMENT.** And measurement rule 12
applies with full force in the usual direction: **Flycast's ADPCM is a
reimplementation, so Flycast agreeing with either hypothesis is evidence about
Flycast.** The experiment is a burn: a loop body that is bit-identical at LSA
and past LEA, preceded by a loud burst before LSA so the two hypotheses start
the second pass from very different states; carried state gives identical
loops, reset state gives an attack transient every period. Run it in mode 3 and
mode 2 on the same data.

⭐ **AND THE ROBUST ANSWER MAKES THE QUESTION MOOT**: encode looped instruments
so the state converges before LSA (a short constant-amplitude lead-in does it,
since `step_size` is clamped to [127, 24576] and the predictor is leaky). Build
it that way regardless of what the burn says.

---

## 7. The codec, and an asymmetry in KOS nobody has resolved

`tools/dcaudio/aica_adpcm.py`. Reference is KOS `utils/wav2adpcm/wav2adpcm.c`
(superctr's public-domain `ymz_codec.c`; AICA ADPCM is YMZ280B with the nibbles
swapped). 4 bits/sample, **no block structure at all** — no headers, no
periodic resets, state runs the whole stream.

```
step_table[8] = {230,230,230,230,307,409,512,614}   indexed by MAGNITUDE alone
init: history = 0, step_size = 127
diff  = clamp(((1 + (delta<<1)) * step) >> 3, 0, 32767)
step' = clamp((step_table[delta] * step) >> 8, 127, 24576)
history = clamp(history -+ diff, -32768, 32767)
```

⚠️ **NIBBLE ORDER IS LOW-FIRST, AND KOS GOT IT WRONG UNTIL RECENTLY.** Sample 0
is the low nibble of byte 0. Older `wav2adpcm` output has them swapped, which
shifts every sample by one and drops the last. Any pre-existing `.adpcm` asset
is suspect.

🔴 **KOS'S OWN ENCODER AND DECODER DISAGREE.** `adpcm2pcm` applies a leaky
high-pass `history = history * 254 / 256` before every step; `pcm2adpcm` does
not model it. Only one can be the hardware, and nothing in KOS settles which.
`aica_adpcm.py` makes it the `leak` parameter rather than picking silently;
the default is **no leak**, on the grounds that KOS's *encoder* is what every
shipped DC ADPCM asset went through and those sound correct. The two differ
audibly only on sustained near-DC content — which is exactly a held instrument
loop. **Unresolved; same burn can settle it.**

### Encoder quality — a measurement that contradicts the obvious expectation

An exhaustive 16-way per-sample search was expected to be free quality over
KOS's open-loop arithmetic quantiser. **It is a wash.** Five real bank samples,
round-trip SNR:

| n | exhaustive | KOS open-loop |
|---:|---:|---:|
| 3,857 | 22.09 dB | 22.07 dB |
| 7,601 | 14.01 dB | 13.74 dB |
| 3,536 | 15.96 dB | **16.08 dB** |
| 2,033 | 28.25 dB | 28.01 dB |
| 5,200 | 23.46 dB | 23.36 dB |

Inside ±0.3 dB and it **loses** on one. A per-sample greedy search is myopic —
the code that minimises this sample's error also sets `step_size` for the next.
Beating it needs lookahead (Viterbi over the step-size state), which is a real
project. **Do not re-propose "search harder" without evidence that 4-bit
quality is what hurts this port.**

---

## 8. 🔴 The runtime design changes: DO NOT drive voices through KOS's queue

This is the most important finding for `dc/src/dc_aica.c`, and it contradicts
the obvious approach.

`kb/audio-stage-b-aica.md` §5.1's design keeps jaudio's arbitrary `envdat`
envelopes on the SH-4 (correct — AICA's 4-stage AR/D1R/D2R/RR hardware EG
cannot represent `ADSR_HANG`/`ADSR_GOTO`/`ADSR_RESTART`) and writes each
voice's volume once per update, **4 × 57 Hz = 229 Hz**. Routing that through
KOS's `AICA_CMD_CHAN` queue does not work:

- 🔴 **The queue has NO overflow check.** `snd_sh4_to_aica` (`snd_iface.c:
  76-117`) never compares `head` against `tail`. Outrun the ARM and `head` laps
  `tail`, silently overwriting unread packets — and the ARM's `while(head !=
  tail)` then walks the whole 32 KB ring reinterpreting garbage as commands.
  No error return, no recovery. Capacity is 341 channel commands in flight.
- 🔴 **Service latency is ~2.3–2.7 ms (~370–430 Hz).** The ARM main loop is
  `aica_get_pos()` for all 64 channels, then drain, then `timer_wait(10)` at
  4,410 Hz (`main.c:214-225`, `aica.c:47`). A 229 Hz update tick is *just*
  inside that, with no margin — updates land in irregular ~2.3 ms clumps.
- ⚠️ **`AICA_CH_CMD_UPDATE` honours only FREQ / VOL / PAN** (`aica_comm.h:
  145-149`). There is no update flag for loop points, sample address or format
  — changing any of those needs a full `START`, which key-offs and key-ons.

✅ **THE DESIGN THAT WORKS**: drive volume/pan/pitch by **direct G2 register
writes with SH-4-side shadows**, and use the ARM queue only for key-on/key-off
and voice setup, where its latency and full-reprogram semantics are what you
actually want. KOS itself reads channel registers directly from the SH-4 while
the ARM driver runs (`snd_iface.c:217`), and `crt0.s:50-62` handles bus
arbitration, so this is an architected path.

```c
{   g2_lock_scoped();                       /* ONE lock for the whole batch */
    for (v = 0; v < nvoices; v++) {
        if ((v & 7) == 0) g2_fifo_wait();   /* the FIFO is 32 bytes */
        g2_write_32_raw(CHNREGADDR(ch[v], 0x28), shadow[v]);
    }
}
```

Never `g2_write_32()` per voice — each takes a full lock (IRQ disable + three
DMA-suspend register writes + FIFO drain). One lock per batch is 229 locks/s
and 5,496 raw writes/s.

### Register map (offsets within the `0x80`-byte per-channel block)

Channel block base **`0x00700000`** (SH-4 P2 `0xA0700000`), stride **`0x80`**,
64 channels. ⚠️ `0x2800` is the *common* register area, not the channels —
easy to conflate.

| off | field |
|---|---|
| `0x00` | b15 `KEYONEX`, b14 `KEYONB`, b9 loop enable, b8-7 format (`PCMS`), b6-0 SA[22:16] |
| `0x04` | SA[15:0] |
| `0x08` / `0x0C` | LSA / LEA — **loop start/end IN SAMPLES for every format** |
| `0x18` | pitch: b14-11 OCT (signed), b10-0 FNS; `freq = 44100 · 2^OCT · (1 + FNS/1024)` |
| `0x24` | byte 36 `DIPAN` (b4 = side, b3-0 magnitude), byte 37 `DISDL` |
| `0x28` | byte 40 LPF control, byte 41 **`TL` — the volume byte**, 0 loudest |

⚠️ **Volume is logarithmic** — `logs[i] = 16·log2(255/i)` (`aica.c:65-86`). A
linear per-voice gain must go through that table.
⚠️ **KOS touches AICA registers from the SH-4 with 32-bit accesses only.**
8-bit G2 writes to registers are unverified. Use 32-bit read-modify-write
against a shadow — `0x28` holds LPF *and* TL in one dword, so a shadow is
required anyway.
🔴 **CA (`0x2814`) is unreadable from the SH-4 by construction** — the ARM
stomps MSLC 64 times every ~2.3 ms in `aica_get_pos`. Read play positions from
the mirror at `AICA_MEM_CHANNELS` (`0x020000 + ch*64`), which is what
`dc_aica_pos()` in `dc_audio.c:348` already does.

### Channel allocation

`snd_stream` holds hardware channels **0 and 1** on a clean boot (allocation
order, not a guarantee — `snd_stream_alloc`, `snd_stream.c:407-419`). Claim
your own with **`snd_sfx_chn_alloc()`** (`sfxmgr.h:257`), which sets a bit in
`sfx_inuse` so `find_free_channel()` and `snd_sfx_stop_all()` both skip them.
⚠️ `spu_reset_chans()` (`spu.c:253`, called from `spu_enable`/`spu_disable`)
and the ARM's `aica_init()` stomp all 64 regardless — do not call `snd_init` or
`spu_*` after voices are up.

### Sound RAM

2,097,152 total − 196,608 KOS reserve = **1,900,544 B allocatable**
(`snd_mem.c:100-103`, `aica_cmd_iface.h:37`). ✅ The kb's figures are exact.
⚠️ `snd_mem_available()` returns the **largest free block**, not the total.
⚠️ ~80 KB of the reserve is dead space, but the two 32 KB queues and the
channel mirror sit at fixed addresses baked into the prebuilt ARM blob, so
reclaiming it needs a rebuilt driver. A lever, not a recommendation.

---

## 9. The residency picture

```
249 sequences   median 50,628 B   mean 74,692 B   max 3,416,862 B
247/249 reference exactly one soundfont
247/249 fit in 1,900,544 B on their own
```

**Only two sequences do not fit**, and both are bank-153 material:

| seq | cost | banks |
|---|---:|---|
| 242 | 3,416,862 B | 2, 155, 154, 153 |
| 247 | 1,971,002 B | 153 |

Both are dominated by the same 19 over-limit samples (§4). Residency is
genuinely tractable: load per sequence start on the existing
`Nas_PreLoadSeq`/`Nas_PreLoadBank` path (`system.c:595`/`:578`), LRU over
**sample ids, not banks**.

⚠️ **A sequence's working set is the UNION of its banks' samples, not the sum**
— soundfonts share samples. `census.py` computes the union.

---

## 10. What is NOT done

- **No encoder output is packed yet.** `aica_adpcm.py` encodes and decodes and
  is measured, but nothing writes a bank file or a runtime-consumable pack.
- **No runtime.** `dc/src/dc_aica.c` does not exist. §8 is its design, not its
  implementation.
- **Reverb has no home.** The seam deletes `Nas_CpuFX` and the aux-buffer bus,
  but once AICA mixes there is no software output left to apply reverb to.
  `kb/audio-stage-b-aica.md` offers "an AICA DSP send" in one line and never
  designs it. A first working offload ships with reverb DROPPED, which is an
  audible change, not a transparent one. Nobody has costed it.
- **The saving is not bounded.** Deleting `RspStart` removes 14.1 ms of a
  71.9 ms frame, but the seam does not delete all 17.2 ms of the audio family:
  `Nas_MySeqMain`, `__Nas_PushDrvReg`, the 229 Hz envelope writes and a
  residency manager all stay on the SH-4, and G2 traffic is new. **No one has
  estimated the residual.**
- **Pitch mapping unverified.** `frequency_fixed_point` (u16 Q15, relative to
  the internal mix rate) → OCT/FNS is arithmetic nobody has written or checked.
