# `tools/dcaudio` — the host side of the AICA audio offload

The findings, and what they changed, are **`kb/audio-aica-offload.md`**. This
file is how to run the thing.

```bash
python3 tools/dcaudio/census.py                      # the residency report
python3 tools/dcaudio/census.py --json /tmp/aica.json # + the sample manifest
python3 tools/dcaudio/tests/test_vadpcm.py           # the decoder's falsifier
```

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
- ⚠️ **The manifest is a derived artefact and is not committed.** Regenerate
  it; do not check it in.

## Not done

No packed bank is emitted yet, and there is no runtime consumer
(`dc/src/dc_aica.c` does not exist). `kb/audio-aica-offload.md` §8 is the
runtime design — in particular, **voices must NOT be driven through KOS's ARM7
command queue**, which has no overflow check and services at only ~430 Hz.
