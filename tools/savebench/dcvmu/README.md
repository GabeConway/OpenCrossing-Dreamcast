# tools/savebench/dcvmu — proving the VMU save backend on a real (emulated) VMU

`savebench.py` in the parent directory models the save on the host. This
directory does the other half: it boots `dc/src/dc_card.c`'s VMU backend on a
Dreamcast, writes to a VMU, reads it back, and then re-verifies the resulting
flash image from the host with an independent parser.

Results are in [`kb/save-budget.md` §7](../../../kb/save-budget.md).

## Why it is not just part of the game build

The game image is 21.4 MB against a 16 MB machine (`kb/STATE.md`), so it does
not execute a single instruction and prints **zero bytes**. There is no way to
observe the save path from a game build until the RAM problem is closed.
`dcvmu.elf` is 2.3 MB, boots in Flycast, and runs the *same* `dc_card.c` — the
Makefile compiles the real file with `-DDC_CARD_STANDALONE`, which swaps
`dc_platform.h` for a ten-line shim and drops the `CARD*` wrappers. It is not a
copy. When the image fits, the identical `dc_card_selftest()` runs from
`CARDInit()` with no code change.

## Files

| file | what it is |
|---|---|
| `main.c` | guest program: enumerate VMUs, run `dc_card_selftest()`, then round-trip three payload sizes and report byte-exactness and timing |
| `Makefile` | builds `main.c` + the real `dc/src/dc_card.c`, links `-lz` (kos-ports zlib, same as `dc/Makefile`) |
| `build.sh` | host entry point: copies sources under `$HOME`, builds in the SDK container, emits `dcvmu.cdi` + the `.src.json` sidecar `crash.sh` needs |
| `vmu_extract.py` | **host-side verifier.** Walks the VMU root/FAT/directory of a 128 KB flash image, recomputes the VMS CRC16, parses the OCS1 container, inflates it and checks both crc32s. A guest that lies and a host parser that lies the same way is not a plausible pair of bugs. |

## Usage

```bash
bash tools/savebench/dcvmu/build.sh
bash harness/dc/smoke.sh ~/.cache/oc-dc-harness/dcvmu/dcvmu.cdi --timeout 300
pkill -f Flycast || true          # it steals focus on a shared desktop

# then verify the flash image the run left behind, on the host
python3 tools/savebench/dcvmu/vmu_extract.py \
  ~/.cache/oc-dc-harness/runs/<run>/home/.flycast/data/vmu_save_A1.bin
```

`--timeout 300` is not paranoia: Flycast models the maple poll cadence, so a
69-block write takes 6.5 s of emulated time and the whole run is ~25 s of VMU
traffic. A 90 s timeout cuts the last round-trip off mid-write.

Kill switches pass through:

```bash
DCVMU_DEFS=-DDC_CARD_NO_COMPRESS=1 bash tools/savebench/dcvmu/build.sh
DCVMU_DEFS=-DDC_CARD_BENCH=1       bash tools/savebench/dcvmu/build.sh
```

## What a passing run looks like

```
MARK:VMU_COUNT 2
[DC/CARD] SELFTEST PASS: 8192 B round-tripped byte-exact (packed 3528 B = 7 blocks, 3% of a VMU)
[DC/CARD] SELFTEST foreign-file: rc=-6 (file is not an OpenCrossing save) — PASS
[DC/CARD] SELFTEST corrupt-file: rc=-5 (corrupt (CRC mismatch)) — PASS
[DC/CARD] SELFTEST missing-file: rc=-2 (no save file on that VMU) — PASS
[DC/CARD] SELFTEST vmu-full: rc=-3 (VMU full), free 198 -> 198 — PASS (nothing written)
ASSERT ok sector8k
ASSERT ok keepmail
ASSERT ok vmufull100k
OC-DC-HARNESS-END rc=0
```

`smoke.sh` still exits 1 with `run_reached_end_marker` failing only if the run
is cut short; with a long enough timeout this program *does* reach the end
marker, unlike the game build, so `end_rc=0` is a real check here.

## Caveats that must travel with any number this produces

- **Flycast is not hardware.** The per-block cost it reports (84.6 ms/block
  write) happens to land within 1.3% of the pessimistic bound derived from
  KOS's `vmufs.c`, which is good evidence the model is right — but a real VMU
  is the only thing that settles it.
- **The compression ratios are of a synthetic three-regime test pattern**
  (a third zeros, a third cyclic text, a third PRNG), not of a save. They say
  the codec path works; they say nothing about how a real town compresses.
  That still needs a real `.gci` (`kb/save-budget.md` §6.1).
