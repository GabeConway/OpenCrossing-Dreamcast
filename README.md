# OpenCrossing-Dreamcast

A native Sega Dreamcast port of Animal Crossing (GameCube), built on the
[ACreTeam decompilation](https://github.com/ACreTeam/ac-decomp). Not an
emulator — the decompiled game is recompiled for SH-4 and its GameCube graphics
API is reimplemented on the PowerVR.

Target is a stock retail console: 16 MB RAM, no shaders, booting off a burned
CD-R.

## ⚠️ Work in progress — not a playable game

It boots on real hardware and you can walk the town with music playing. That's
about it. Do not expect to play Animal Crossing on this.

**Working:** boots on a retail Dreamcast (MIL-CD unit) and in Flycast. Title →
intro → train → town → Nook's shop → house interiors. Music and sound effects.
All 3,942 decomp objects build for sh-elf with no exclusions.

**Not working:** frame rate is ~16–23 FPS in Flycast and worse on hardware,
which is the main problem. No saving — the VMU storage layer exists but the
game's save path isn't wired to it. No villagers walking around; the game gates
them behind Nook's first job and nobody has played that far at this frame rate.
Some models and materials are missing, and the name-entry keyboard renders
black.

Current numbers: [`kb/STATE.md`](kb/STATE.md). What's broken and in what order:
[`kb/RESUME.md`](kb/RESUME.md).

**Game version:** `GAFE01` — Animal Crossing (USA), Rev 0. You supply your own
disc image. Nothing ships here.

## Building

Needs Docker, Python 3.9+, and your own `GAFE01` ISO (1,459,978,240 bytes). The
SH-4 toolchain, KallistiOS and the disc tools all live in the container.

> The container is hardcoded to `linux/arm64` and has only been run on an
> Apple-silicon Mac under colima. On x86-64 it'll fall back to qemu. A native
> amd64 image should work but has never been tried — start by dropping the
> `--platform` flags in `dc/build-dc.sh` and `dc/build-dc-image.sh`.

```bash
git clone https://github.com/GabeConway/OpenCrossing-Dreamcast
cd OpenCrossing-Dreamcast

# once, ~27 min: sh-elf-gcc + KallistiOS + mkdcdisc
bash dc/build-dc-image.sh

# pull the assets out of your ISO and flatten them into a disc root
make -C tools/dcasset extract ISO="/path/to/Animal Crossing.iso"
bash dc/stage-disc.sh /tmp/opencrossing-dc/discroot ~/.cache/oc-dc-discroot

# build
DC_STUB_KEEP="$(grep -v '^#' tools/dcstub/keeplist-full.txt | paste -sd: -)" \
DC_DISC_ROOT=~/.cache/oc-dc-discroot DC_ASSET_STUB=1 \
DC_ARAM_WINDOW=1048576 DC_ARENA_BYTES=1200000 \
DC_AUDIO_SCENES=all DC_AUDIO_DISC_FRAMES=8 DC_AUDIO_VOICES=12 \
bash dc/build-dc.sh
```

Use that build line, not a bare `dc/build-dc.sh`. Those variables aren't tuning
— without `DC_STUB_KEEP` you get an image with about 53 KB of assets in it that
boots, renders, and reports zero errors while being almost entirely empty.
Check the build log says something like:

```
dc_stub_keep.inc: 3261 table rows + 2 .c_inc rows + 460 per-file init fns (2,929,360 B)
```

Output is `dc/build/OpenCrossing.cdi`. Open it in Flycast, or add
`DC_CDI_PAD=1 DC_CONSOLE_MUTE=1` and burn it to a CD-R for hardware.

Every target and knob: [`BUILDING-DC.md`](BUILDING-DC.md).

**Don't distribute a built image** — it contains Nintendo's assets.

## Contributing

Frame rate is the top item and everything else is downstream of it. Other
self-contained starts: the VMU save path, the 27 TEV configs that need a
predicate narrowed, and hardware profiling (implemented, needs someone with a
console and a CD-R).

The port lives in `dc/`; `src/` is the decompilation and is never edited to
make it compile — compat fixes go in the force-included
`dc/include/dc_prelude.h`. `kb/` is the knowledge base, dead ends included.
Start at [`kb/RESUME.md`](kb/RESUME.md), and read
[`kb/closed.md`](kb/closed.md) and [`kb/traps.md`](kb/traps.md) before opening
a PR. Renderer changes are judged on a matched-frame screenshot pair, not on
counters.

## Hall of Heroes

This port stands on the shoulders of:

- **[ACreTeam / ac-decomp](https://github.com/ACreTeam/ac-decomp)** — the
  complete C decompilation of Animal Crossing. Without it there is no port.
- **[Cuyler36](https://github.com/Cuyler36)** — the Ghidra GameCube loader,
  the save editor, and the reverse-engineering groundwork the decomp grew
  from; also the unlocked-FPS work upstream.
- **[flyngmt / ACGC-PC-Port](https://github.com/flyngmt/ACGC-PC-Port)** — the
  original PC port: the GX→OpenGL layer and the keep-emu64 architecture this
  project inherits wholesale.
- **[Dia2809](https://github.com/Dia2809/ACGC-PC-Port)** — Linux support, the
  OpenGL ES renderer, and the ARM branches: the base the handheld port used.
- **[Falco Girgis](https://github.com/gyrovorbis)** — KallistiOS and
  [sh4zam](https://github.com/gyrovorbis/sh4zam), the SH-4 math this port's
  transform path runs on, plus direction given directly to this project.
- **[OpenCrossing-Anbernic](https://github.com/GabeConway/OpenCrossing-Anbernic)**
  — the low-end-hardware port this one forked from.

The same names, in the same order, are on the boot screen.

## Legal

Not affiliated with, endorsed by, or sponsored by Nintendo or Sega. "Animal
Crossing" and related marks are Nintendo's, used here only to describe
compatibility.

No disc image, ROM dump, or extracted asset ships in this repository. Supply
your own legally obtained copy; the build reads it and never redistributes it.
Images you build are for personal use only.

`src/` is a decompilation — C source reconstructed from the retail game,
published by [ACreTeam](https://github.com/ACreTeam/ac-decomp) under CC0.
Nintendo authored neither it nor its license. The port layers (`dc/`, `tools/`,
`harness/dc/`, `kb/`, `pc/`) are MIT. See [LICENSE](LICENSE).

## AI use

Claude (Anthropic) was used heavily in the making of this project.
