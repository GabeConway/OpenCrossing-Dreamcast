# OpenCrossing-Dreamcast

A native Sega Dreamcast port of Animal Crossing (GameCube), built on the
[ACreTeam decompilation](https://github.com/ACreTeam/ac-decomp) and the
[OpenCrossing-Anbernic](https://github.com/GabeConway/OpenCrossing-Anbernic)
low-end-hardware port.

**Status: M2 in progress.** All 3917 TUs build and link for sh-elf; a stub image boots in Flycast and renders the title screen — see `kb/STATE.md` — see [PLAN.md](PLAN.md) for the
full technical plan and `kb/` for the research it is based on.

Supported game version (planned): `GAFE01` — Animal Crossing (USA), Rev 0.

> ## Disclaimer
>
> This project is not affiliated with, endorsed by, or sponsored by Nintendo
> or Sega. "Animal Crossing" and all related names and marks are trademarks of
> Nintendo; they are used here only to factually describe compatibility.
> No game assets, ROM contents, or original code ship with this repository —
> you must supply your own legally obtained disc image of a game you own.
> The build process produces a disc image for personal use only; do not
> distribute built images, as they contain Nintendo-copyrighted assets.

## The idea

The Anbernic port proved the decompiled game runs well on weak 32-bit ARM
hardware. The Dreamcast is one step further down: SH-4 @ 200 MHz, 16 MB RAM,
a fixed-function PowerVR GPU with no shaders — and one step sideways: it is
little-endian ILP32 like ARM, so all of the endianness and 32-bit-pointer
work transfers unchanged. Recent Dreamcast homebrew (GTA III via dca3,
Super Mario 64, Doom 64, WipEout) shows games of this class are viable with
the right asset pipeline and renderer.

Planned end state: clone repo, drop your own GameCube ISO in `rom/`, run the
builder, get a self-booting CDI for a real Dreamcast (CD-R or ODE) or the
Flycast emulator.

## Repository layout (planned)

```
PLAN.md          — the port plan (read this first)
kb/              — knowledge base: research notes, decisions, device facts
dc/              — Dreamcast platform layer (replaces pc/ from the base repo)
tools/           — host-side asset extraction / conversion / disc build
src/             — vendored decomp game code (unchanged where possible)
harness/         — emulator smoke tests (never contains ROM material)
```

## Credits

This project stands on the shoulders of:

- **[ACreTeam / ac-decomp](https://github.com/ACreTeam/ac-decomp)** — the complete
  C decompilation of Animal Crossing that makes any of this possible.
- **[flyngmt/ACGC-PC-Port](https://github.com/flyngmt/ACGC-PC-Port)** — the
  original PC port of the decompilation (GX→OpenGL translation layer).
- **[Dia2809's fork](https://github.com/Dia2809/ACGC-PC-Port)** — Linux support,
  the OpenGL ES renderer, and the ARM branches.
- **[GabeConway/OpenCrossing-Anbernic](https://github.com/GabeConway/OpenCrossing-Anbernic)**
  — the low-end handheld port this project is bootstrapped from.
- **[KallistiOS](https://github.com/KallistiOS/KallistiOS)** and the Dreamcast
  homebrew community — especially the dca3, sm64-dc, and doom64-dc projects,
  whose published techniques inform most of this plan.

Animal Crossing is © Nintendo. This project is not affiliated with or endorsed
by Nintendo or Sega, and distributes none of their assets.

AI tools (Claude) are used in the porting and planning work (platform code
only, not the decompiled game code).
