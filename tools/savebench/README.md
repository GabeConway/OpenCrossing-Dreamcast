# tools/savebench — VMU save-budget benchmark

Answers one question: **does an Animal Crossing save fit on a Dreamcast VMU?**
Results and the recommendation live in [`kb/save-budget.md`](../../kb/save-budget.md);
this directory is the thing that produced them, so the numbers can be
re-derived whenever the save layout changes.

The top level is host-side only: nothing in *this* directory runs on the
Dreamcast and nothing here reads the game ISO. **[`dcvmu/`](dcvmu/README.md) is
the exception and the other half of the story** — it boots
`dc/src/dc_card.c`'s VMU backend in Flycast and measures what the hardware
actually does. Read its README before trusting any write-cost or throughput
number; the estimates in `kb/save-budget.md` §5 were superseded by it (§7).

## Files

| file | what it is |
|---|---|
| `dcvmu/` | **guest-side harness.** Boots the real VMU backend, round-trips payloads on a real (emulated) VMU, and re-verifies the resulting flash image from the host with an independent FAT/CRC parser |
| `save_layout_probe.c` | Compiles against the vendored decomp headers and prints every `sizeof`/`offsetof` in the save chain. **This is the ground truth.** |
| `save_layout.py` | The region model: every byte of `Save_t` and the three keep-blocks, tagged with a field group and a content kind. Self-validating (`validate()` asserts the regions tile each struct exactly). |
| `gen_save.py` | Synthetic save generator. Real struct layout, synthetic contents, four fill profiles. |
| `savebench.py` | Driver: compresses, compares against the VMU budget, evaluates the PLAN.md §6 options. |

## Re-measuring after a layout change

1. **Re-run the probe.** It must be built for a 32-bit ABI — a native macOS/x86-64
   build inflates `Save_t` from 148128 to 153216 bytes because 64-bit alignment
   changes the padding. Use the armhf container the base port already ships
   (ARM EABI has the same 8-byte `u64` alignment as the GameCube PPC ABI and as
   SH-4):

   ```bash
   cd /Users/gabe/Documents/GitHub/OpenCrossing-Dreamcast
   docker run --rm --platform linux/arm/v7 -v "$PWD":/work acgc-smoke-deps:armhf bash -c '
     gcc -DTARGET_PC -DVERSION=0 -DF3DEX_GBI_2 -DNDEBUG -D_LANGUAGE_C \
         -DBUILD_USER=\"x\" -I /work/include -I /work/src -I /work \
         -I /work/pc/include -w -o /tmp/probe /work/tools/savebench/save_layout_probe.c \
     && /tmp/probe'
   ```

   Sanity check: the printed offsets must reproduce the `/* 0x...... */`
   comments in `include/m_common_data.h`. If they do not, the ABI is wrong and
   every number downstream is wrong.

2. **Update `save_layout.py`** (`SIZEOF`, `COUNTS`, and any moved region).
   `python3 save_layout.py` re-validates the tiling and fails loudly on a gap.

3. **Re-run the bench** and update `kb/save-budget.md`.

## Usage

```bash
cd tools/savebench

python3 save_layout.py              # validate the region model
python3 savebench.py                # all four synthetic profiles, all codecs
python3 savebench.py --profile full
python3 savebench.py --groups full  # which field groups cost what
python3 savebench.py --trim full    # PLAN.md §6(c): content trimming
python3 savebench.py --delta full   # PLAN.md §6(d): base + journal
python3 savebench.py --dict full    # CD-resident preset dictionary (spoiler: no)
python3 savebench.py --cost full    # compression + VMU write time
python3 savebench.py --recommend    # the shipping proposal, all profiles
```

### With real saves (preferred)

Synthetic data is a model, not evidence. As soon as a real late-game
`DobutsunomoriP_MURA.gci` exists (467008 bytes: 64 B GCI header + 0x72000):

```bash
python3 savebench.py --gci /path/to/DobutsunomoriP_MURA.gci
```

`--gci` pulls the same four blocks out of the real file that the generator
synthesises, so the output tables are directly comparable. **Do not copy user
save data into the repo** — pass an absolute path to wherever it lives.

## Fill profiles

| profile | meaning |
|---|---|
| `fresh` | new town just past the tutorial |
| `typical` | one well-played player, a season in |
| `full` | 4 players, every design/letter/diary slot used, plausible hand-drawn art |
| `adversarial` | same occupancy, but every user-authored byte drawn uniformly at random. Not realistic — it is the incompressible ceiling a determined player could in principle author, and the number you size for if a failed save is unacceptable. |

## Known gaps

- No real save corpus was available on the dev machine (searched
  `OpenCrossing-Anbernic/**`, `save/card_a`, `save/card_b`, harness output, git
  history — all empty). Every ratio below `--gci` is **synthetic**.
- Design-pattern and letter entropy are the two model parameters the real
  answer is most sensitive to. Replace them with measurements first.
- The SH-4 compression time in `--cost` is a host-throughput extrapolation with
  a hand-picked scale factor, not a measurement. Same for the VMU write time,
  which is bracketed between the flash floor and a KOS-maple-cadence estimate.
  Both are flagged `UNVERIFIED` in the output and must be replaced with
  Flycast/hardware numbers at M3.
