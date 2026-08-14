# Building & development

Developer notes. If you just want to play, grab a
[release zip](../../releases/latest) instead — see the install guide in
[README.md](README.md).

## Device build (armhf, Docker)

Cross-build the 32-bit ARM binary from any machine:

```bash
docker run --rm --platform linux/arm/v7 \
  -v "$PWD":/work \
  debian:bookworm bash /work/pc/build-armhf-docker.sh
```

Output: `pc/build-armhf/bin/AnimalCrossing` (ELF 32-bit ARM EABI5 hard-float).
The build dir persists on the host mount, so rebuilds are incremental.

**Colima gotcha:** colima's docker only has the legacy builder, which
*silently ignores* `--platform` when an arm64 base image is already cached —
you end up with an arm64 build that looks fine until it hits the device.
Always verify the container arch:

```bash
docker run --rm --platform linux/arm/v7 debian:bookworm uname -m   # must print: armv7l
```

## Desktop dev build (macOS / Linux)

```bash
./build_pc.sh
```

Needs SDL2 and a C toolchain. Output: `pc/build/bin/AnimalCrossing`. Fastest
iteration loop for gameplay/logic work (64-bit, desktop GL).

## Smoke tests (harness/)

Headless Docker boot tests that launch the game under Xvfb and check it gets
past disc load:

```bash
./harness/smoke.sh armhf    # exact device ABI (slow, QEMU)
./harness/smoke.sh arm64    # near-native, same GLES class as device
```

Put your GAFE01 USA disc image at `harness/rom/Animal Crossing.iso` — the
`harness/rom/` directory is git-ignored and its contents must **never** be
committed. Logs land in `harness/out/<arch>-smoke.log`. See
`harness/README.md` for the full three-tier setup (including one-time
colima/binfmt install).

## Cutting a release

⚠️ **There is no release automation in this repository, deliberately.**

An inherited `.github/workflows/release.yml` used to build an armhf binary
under QEMU on any `v*` tag and publish it as a public GitHub release, with
`release.sh` posting the same zip to a Discord webhook. Both were removed on
2026-08-13. They came from the Anbernic handheld port, they never built
anything for the Dreamcast target, and a workflow that publishes a compiled
binary of the decompiled game is not something that should fire by accident on
a tag.

Build images locally from your own disc — `dc/build-dc.sh`, see
[`BUILDING-DC.md`](BUILDING-DC.md) — and do not distribute them.
