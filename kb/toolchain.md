# Toolchain — what it is and the facts that cost something

The SDK image is **built and working**; `dc/Dockerfile` is the recipe and the
authority. `BUILDING-DC.md` is the live build documentation. This file keeps
only the decisions and the non-obvious facts, which the six M0 design docs used
to carry (deleted 2026-08-09; recover with
`git log --diff-filter=D -- 'kb/toolchain-*.md' kb/design-toolchain.md`).

## The decision

Build the cross toolchain from source with **`kos-chain`** inside a pinned
two-stage Docker image, rather than using a prebuilt sh-elf. Reasons: KOS 2.3
wants a matched GCC, the prebuilt images available were older, and pinning the
SHAs makes the image reproducible. Image tag `opencrossing-dc:sdk`, built by
`dc/build-dc-image.sh` (~27 min cold — **do not rebuild casually**).

## The facts worth keeping

- **`char` is SIGNED on sh-elf.** An early doc claimed unsigned; it is wrong.
- **Hard float, single precision.** `-m4-single-only`, matching KOS.
- **Docker runs under colima on an Apple M4** (arm64, 10 cores, 24 GB) and this
  host has **no BuildKit** — use `DOCKER_BUILDKIT=0`, never pass `--progress`.
- ⚠️ **colima only bind-mounts paths under `$HOME`.** A volume outside it mounts
  empty, silently.
- ⚠️ **Inside the SDK image use `bash -c`, never `bash -lc`** (`kb/traps.md`).
- **qemu emulation of x86 tooling ran 17–23× slower and hit an ICE**, which is
  why the image is native arm64.
- **A full rebuild is 96 seconds** (measured 2026-08-06). Plans that treat a
  rebuild as expensive are costing a guess.
- **`mkdcdisc` needs `-N` for emulator runs** and `DC_CDI_PAD=1` for burns.

Everything mechanical lives in `kb/traps.md` §"Docker / SDK image" and
§"Compile / link".
