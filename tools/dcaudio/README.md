# `tools/dcaudio` — the host side of the AICA audio offload

The findings, and what they changed, are **`kb/audio-aica-offload.md`**. This
file is how to run the thing.

```bash
python3 tools/dcaudio/census.py                       # the residency report
python3 tools/dcaudio/census.py --json /tmp/aica.json # + the sample manifest
python3 tools/dcaudio/tests/test_vadpcm.py            # the decoder's falsifier
python3 tools/dcaudio/pack.py --out /tmp/aicabank.pak           # build the bank
python3 tools/dcaudio/pack.py --out /tmp/aicabank.pak --verify  # check it
```

The full bank packs in ~11 s to **4,753,376 B** (1,133 samples encoded, 24
excluded as too long for a hardware channel).

`census.py` needs two inputs and defaults to finding both:

| input | default | note |
|---|---|---|
| `audiorom.img` | `~/.cache/oc-dc-discroot/audiorom.img` | `--img`; produced by `tools/dcasset` |
| `audioheaders.c` | `src/static/jaudio_NES/game/audioheaders.c` | `--headers` |

⚠️ **Both are required, and that is not an accident.** `audiorom.img` carries
no index of any kind — every table saying where anything lives is a static C
array compiled into the game. See `kb/audio-aica-offload.md` §2.

## The modules

| file | what |
|---|---|
| `vadpcm.py` | the N64 VADPCM decoder, transliterated from `rspsim.c:107`'s `TARGET_PC` arm. **Bit-exact against the shipped C and tested that way** |
| `audiorom.py` | parses `audioheaders.c`, resolves aliases, walks soundfont → instrument/drum → `smzwavetable` → sample, dedupes |
| `aica_adpcm.py` | Yamaha/AICA 4-bit ADPCM encode + decode, round-trip SNR, and `analyse_loop()` — the loop-state convergence measurement |
| `census.py` | the report and the manifest |
| `pack.py` | builds `aicabank.pak` (device_addr-sorted index + 32-byte-aligned ADPCM payloads) and, with `--verify`, re-derives it from the source bank and diffs it |
| `tests/ref_adpcm.c` | the oracle: `A_CMD_ADPCM`'s frame loop lifted verbatim out of `rspsim.c` |
| `tests/test_vadpcm.py` | drives both with random frames and diffs them |

## The join key, which is the whole reason this can talk to the runtime

A sample's **`device_addr` is its raw byte offset into `audiorom.img`**.
`Nas_StartDma` computes `aram_offset = device_addr + GetNeosRomTop()`
(`system.c:1299`) and `GetNeosRomTop()` carries no file-offset component
(`dummyrom.c:25-27`). So the runtime recovers the manifest key from a live
sample pointer as `sampleAddr - GetNeosRomTop()`. It is stable across builds.

That is the `device_addr` field in the `--json` manifest.

## Traps

- ⚠️ **Build the oracle `-fwrapv`.** `accu` is s32 and real scale/coefficient
  combinations leave the range; signed overflow is UB, so without `-fwrapv` the
  test measures the host compiler rather than the console. `test_vadpcm.py`
  passes the flag; do not drop it.
- ⚠️ **`--usable` defaults to 1,900,544 B** (2 MB minus KOS's 196,608 B ARM
  reserve). Verified against `snd_mem.c:100-103` and `aica_cmd_iface.h:37`.
- ⚠️ **Every census figure is 4-bit-throughout.** The report also prints the
  what-if-looping-samples-go-8-bit column, which is **4.9× usable** and
  falsifies a kb mitigation. Do not quote the optimistic column alone.
- ⚠️ **A sequence's working set is the UNION of its banks' samples**, not the
  sum — soundfonts share samples.
- ⚠️ **The manifest and `aicabank.pak` are derived artefacts and are not
  committed.** Regenerate them; do not check them in. (`audiorom.img` is ROM
  material — CLAUDE.md §1.)
- ⚠️ **`--verify` must be given the same `--encoder`/`--leak` the pack was
  written with**, or it reports a false mismatch. That has already happened
  once.

## Not done

There is no runtime consumer — `dc/src/dc_aica.c` does not exist, and nothing
here has executed on the console. `kb/audio-aica-offload.md` §8 is the runtime
design; in particular, **voices must NOT be driven through KOS's ARM7 command
queue**, which has no overflow check at all and services at only ~430 Hz.
§11 is why the offload can be built voice-by-voice rather than as one cut, and
the latency-skew objection that comes with it.
