# OpenCrossing-Dreamcast

**A native Sega Dreamcast port of Animal Crossing (GameCube)** — built on the
[ACreTeam decompilation](https://github.com/ACreTeam/ac-decomp), targeting a
**stock, unmodified retail Dreamcast**: SH-4 @ 200 MHz, **16 MB of RAM**, a
fixed-function PowerVR with no shaders, booting from a burned CD-R.

Not an emulator. The decompiled game is recompiled for SH-4 and its GameCube
graphics API is reimplemented against the PowerVR.

> **Status: it boots on real hardware and walks the town, with music.**
> It is not yet a game you can finish — see [What works](#what-works) for an
> honest line-by-line.

<!-- TODO before going public: drop a screenshot or a short capture here.
     `docs/doc_assets/town.png` and reference it as:
     ![The town, running on a retail Dreamcast](docs/doc_assets/town.png) -->

---

## What works

| | |
|---|---|
| ✅ | **Builds completely.** All **3,936** objects of the decomp link for sh-elf with **zero exclusions** — no file is skipped to make the port compile |
| ✅ | **Boots on a retail Dreamcast** from a burned CD-R (MIL-CD unit), and in the Flycast emulator |
| ✅ | **Walks the town.** Title → intro → train → town → Tom Nook → the houses |
| ✅ | **Music and sound effects**, streamed and mixed on the SH-4 |
| ✅ | **Every summer acre, the interiors, the winter set and the gyroids** are in the image |
| ✅ | **Textures stream off the disc on demand** — 6,068 texture arrays are read at bind time instead of living in RAM |
| ⚠️ | **Frame rate.** ~16–23 FPS in Flycast depending on scene; **noticeably slower on real hardware**, and closing that gap is the current focus |
| 🔴 | **No villagers yet.** The roster is generated correctly; nothing constructs the actors |
| 🔴 | **No saving yet.** The VMU storage layer exists; the game's save path is not wired to it |
| 🔴 | **Some geometry and a few materials are still missing** — the name-entry keyboard renders black, and some models are dropped by the residency budget |

Current numbers live in [`kb/STATE.md`](kb/STATE.md); what is still broken, and
in what order, is [`kb/RESUME.md`](kb/RESUME.md) §7.

**Supported game version:** `GAFE01` — Animal Crossing (USA), Rev 0.
You supply your own disc image of a game you own. None ships here.

---

## Why this is hard

The GameCube is not a little machine, and the Dreamcast is the generation
before it:

| | GameCube | Dreamcast | ratio |
|---|---|---|---|
| CPU | PowerPC 750CXe @ 485 MHz | SH-4 @ 200 MHz | ~2.4× |
| Main RAM | 24 MB 1T-SRAM + 16 MB ARAM | **16 MB, total** | ~2.5× |
| GPU | Flipper, 16-stage programmable TEV | PowerVR CLX2, **fixed function** | — |
| Vertex transform | hardware T&L | **on the CPU** | — |
| Texture RAM | 3 MB embedded 1T-SRAM | 8 MB VRAM, twiddled/VQ only | — |
| Save | ~456 KB memory-card file | **~100 KB VMU** | 4.6× |
| Media | 1.5 GB disc, fast seeks | CD-R at ~500 KB/s | — |

Three of those are structural rather than "make it faster":

1. **The TEV has no equivalent.** The game's 101 distinct texture-combine
   configurations were each mapped by hand onto a fixed-function PowerVR
   strategy — see [`kb/tev-map.md`](kb/tev-map.md).
2. **The game does not fit in RAM.** 14,495 assets, 8.8 MB of destination
   arrays alone. The port stubs what it can prove is unused and streams the
   rest off the disc at the moment it is bound.
3. **Vertex transform and lighting run on the SH-4**, so the frame is bound by
   memory traffic, not by arithmetic — measured, and it reshaped the entire
   optimization queue.

The one advantage: the Dreamcast is little-endian ILP32, exactly like the ARM
handheld port this project grew from, so all of the endianness and
32-bit-pointer work transferred unchanged.

---

## How it is built

The architecture is inherited from the PC port and kept deliberately:

```
  src/          the decompiled game, UNMODIFIED
    │           (five #if TARGET_DC branches in four files — that is the whole licence)
    ▼
  emu64         the game's own N64-graphics interpreter, kept as-is
    │
    ▼
  GX API        GameCube graphics calls
    │
    ▼
  dc/           ◄── this port: GX → PowerVR, SH-4 math, KallistiOS platform layer,
                    demand asset loading, AICA audio, VMU save
```

**The hard rule the project runs on: `src/` is never edited to make it
compile.** Compatibility fixes go into one force-included prelude
(`dc/include/dc_prelude.h`). That keeps the decomp mergeable with upstream and
keeps every bug attributable to the port rather than to a local patch.

Layout:

```
dc/              Dreamcast platform layer — the port itself
  src/dc_pvr.c     the PowerVR backend
  src/dc_gx.c      the GX state machine
  src/dc_texpool.c demand texture loading off the disc
  src/dc_audio.c   AICA streaming
  src/dc_card.c    VMU save storage
tools/           host-side: asset extraction, packing, disc build, analysis
harness/         emulator smoke tests and the regression gate
kb/              the knowledge base — research, measurements, dead ends
src/             vendored decomp game code (unchanged)
pc/              the Linux/SDL reference port (reference only, NOT the target)
```

---

## Building

You need **Docker**, **Python 3.9+**, and a legally obtained `GAFE01` disc image
(Animal Crossing, USA Rev 0, 1,459,978,240 bytes). Nothing else — the SH-4
toolchain, KallistiOS and the disc tools all live in the container image.

No ROM material ships with this repository and none ever will. Every step below
reads *your* disc image and writes outside the repo.

### 1. Clone and build the toolchain image — once

```bash
git clone https://github.com/GabeConway/OpenCrossing-Dreamcast
cd OpenCrossing-Dreamcast

bash dc/build-dc-image.sh     # ~27 min: sh-elf-gcc + KallistiOS + mkdcdisc
```

Idempotent, and slow only the first time. Don't rebuild it casually.

### 2. Extract the disc content from your ISO

The game streams its archives, models and audio off the disc at runtime, so it
needs a **disc root** — a flat directory the build hands to `mkdcdisc`:

```bash
make -C tools/dcasset extract ISO="/path/to/Animal Crossing.iso"
bash dc/stage-disc.sh /tmp/opencrossing-dc/discroot ~/.cache/oc-dc-discroot
```

`extract` writes the GameCube shape (`files/`, `sys/`); `stage-disc.sh` flattens
it, because every path the port opens is `/cd/<name>` with no subdirectory.

### 3. Build

⚠️ **Use this line, not a bare `dc/build-dc.sh`.** The environment variables are
not optional tuning — `DC_STUB_KEEP` is what decides whether the game has any
content at all. Leave it out and you get a technically-working image containing
about 53 KB of assets: the title logo and nothing else.

```bash
DC_STUB_KEEP="$(grep -v '^#' tools/dcstub/keeplist-full.txt | paste -sd: -)" \
DC_DISC_ROOT=~/.cache/oc-dc-discroot DC_ASSET_STUB=1 \
DC_ARAM_WINDOW=1048576 DC_ARENA_BYTES=1200000 \
DC_AUDIO_SCENES=all DC_AUDIO_DISC_FRAMES=8 DC_AUDIO_VOICES=12 \
bash dc/build-dc.sh
```

Output is `dc/build/OpenCrossing.cdi` plus the ELF beside it.

**Check one line of the build log.** It is the cheapest possible confirmation
that the keep list actually applied:

```
dc_stub_keep.inc: 3261 table rows + 2 .c_inc rows + 460 per-file init fns (2,929,360 B)
```

If that number is missing or small, stop — the image will boot, render, report
zero errors, hold a steady frame rate, and be almost entirely empty. A stubbed
asset is not a *missing* asset; it is deliberately one byte, so every counter
stays honest and green while the geometry is gone. That failure mode has cost
this project more than one debugging session.

### 4. Run it

- **Flycast:** open `dc/build/OpenCrossing.cdi`.
- **Real hardware:** add `DC_CDI_PAD=1 DC_CONSOLE_MUTE=1` to the build line and
  burn the CDI to a CD-R. A MIL-CD-capable console is required (most pre-2000
  units). The mute is worth ~5 % of the frame — KOS busy-waits on the serial
  FIFO *with no cable attached* — but it also silences crash dumps, so drop it
  when you are triaging rather than playing.

Full reference — every make target, every environment knob, the include-path
order and the troubleshooting list — is [`BUILDING-DC.md`](BUILDING-DC.md).
The build lines used for measurements, including the profiling and screenshot
variants, are in [`kb/RESUME.md`](kb/RESUME.md) §2.

> **Never distribute a built image.** It contains Nintendo's assets. Build your
> own from your own disc.

---

## Performance work

The port is fast enough to walk around and slow enough to be the main problem.
Everything about that is measured rather than guessed, and the measurements are
kept even when they are inconvenient:

- The frame is **memory-bound**, not math-bound. All floating-point vertex work
  is **0.76 ms of a ~29 ms frame** — which demoted every "use the SH-4 matrix
  unit" idea in one measurement.
- A frustum cull at the interpreter's triangle-batch entry took **19.9 ms off a
  69.8 ms frame**.
- Switching the tree from `-O0` to `-Os` plus a reviewed `-O3` hot list took
  the town from **11.6 to 20.0 FPS** and cut 2.8 MB of code.
- ⚠️ **Flycast models no instruction cache, no operand cache and no disc seek
  time.** It can falsify an instruction-count claim; it can never falsify a
  cache-locality one. Real hardware is the verdict, and the current hardware
  deficit is unexplained — the SH-4's instruction cache is **8 KB,
  direct-mapped**, against a hot set 16× that size.

Every optimization ships with a kill switch, and **the default build is the
good build** — a result that lives only in a command line is one unset
environment variable away from being lost.

---

## The knowledge base

`kb/` is the substance of this project. It is written to be read by whoever
picks the work up next, including its dead ends:

| file | what it carries |
|---|---|
| [`kb/RESUME.md`](kb/RESUME.md) | **start here** — state, build lines, the twelve measurement rules, what is broken |
| [`kb/STATE.md`](kb/STATE.md) | the current numbers and the ranked queue |
| [`kb/closed.md`](kb/closed.md) | ideas that are dead, and why — read before proposing one |
| [`kb/traps.md`](kb/traps.md) | mechanical gotchas already paid for |
| [`kb/tev-map.md`](kb/tev-map.md) | all 101 TEV configs → fixed-function strategies |
| [`kb/perf-dc.md`](kb/perf-dc.md) | where the frame goes |
| [`PLAN.md`](PLAN.md) | milestones, the four hard problems, the risk register |

Several figures in `kb/` have been **falsified by later measurement and the
correction left in place on purpose.** An unsourced number in there is a claim,
not a fact.

---

## Contributing

Useful and self-contained places to start:

- **Villager actors** — the roster is built; nothing spawns them.
- **The VMU save path** — the storage layer works; the game's save I/O uses
  stdio on a relative path that does not exist on the console.
- **TEV configs** — 27 of the 101 need one predicate narrowed; the maths is
  already derived.
- **Hardware profiling** — SH-4 performance counters are implemented and need
  someone with a console and a CD-R.

Please read [`kb/traps.md`](kb/traps.md) and
[`kb/closed.md`](kb/closed.md) before opening a PR. Renderer changes are
judged on a matched-frame screenshot pair, not on counters.

---

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

---

## Legal

This project is **not affiliated with, endorsed by, or sponsored by Nintendo or
Sega.** "Animal Crossing" and all related names and marks are trademarks of
Nintendo; they are used here only to factually describe compatibility.

**No game assets, ROM contents, or original game code ship in this
repository.** You must supply your own legally obtained disc image of a game
you own. The build produces a disc image **for personal use only** — do not
distribute built images, as they contain Nintendo-copyrighted assets.

Licensing: see [LICENSE](LICENSE). The decompilation under `src/` is CC0 from
ACreTeam; the port layers are MIT.

AI tools (Claude) are used in the porting and planning work — platform code and
documentation only, not the decompiled game code.
