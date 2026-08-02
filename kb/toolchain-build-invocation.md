# Dreamcast toolchain — build times and how the container is invoked

Measured stage-1 (24 min) and stage-2 (2.5 min) image build times at 4 cores,
and the bind-mount / invocation design the `dc/build-dc*.sh` scripts implement.
Read when a build feels wrong-length or when changing how the container is run.
Practical entry points live in `BUILDING-DC.md`. Tag discipline (**[VERIFIED]** = ran it on this host and read the output, **[UNVERIFIED]** = inferred or read from docs) is unchanged from `kb/design-toolchain.md`; do not upgrade an UNVERIFIED claim without re-measuring.

## 6. Expected build time

Measured on this host, **4 cores** (`makejobs=4`), Debian bookworm arm64,
`stable` profile, Obj-C/Obj-C++ disabled, no `arm-eabi`.

### Stage 1 — sh-elf toolchain (the expensive one, cached forever)

Phase boundaries, wall clock from `docker build` start. Derived from the mtimes
of `utils/kos-chain/logs/*` (the container and host clocks were confirmed
identical). These are *last-write* times per phase log, so read them as "this
phase was finished by": [VERIFIED, ±10 s]

| Phase | Done by |
|---|---|
| apt deps + KOS clone + binutils 2.45.1 | **~1 min** |
| GCC 15.2.0 **pass 1** (C only, no libc) | **~6.6 min** |
| newlib 4.6.0.20260123 | **~7 min** |
| GCC 15.2.0 **pass 2** — full GCC rebuild + libstdc++ × 2 multilibs | **dominant phase** |

Everything before pass 2 completes in **≈ 7 minutes**. Pass 2 rebuilds the entire
compiler with C++ enabled and then builds `libstdc++` twice (once per multilib:
`m4-single` and `m4-single-only`), so it dwarfs the rest.

**Stage 1 total: 1464 s = 24 min 24 s, exit 0.** [VERIFIED — this exact
Dockerfile was built on this host.] Resulting image `dc-toolchain:stable`:
**3.93 GB on disk / 802 MB compressed content.**

Cross-compilers do **not** bootstrap (GCC is built once, not three times), which
is why 24 min is achievable where a native GCC build would take far longer.

### Stage 2 — KOS + kos-ports (GLdc, zlib) + mkdcdisc

**Stage 2 total: 147 s = 2 min 27 s, exit 0.** [VERIFIED — built on this host.]

That is the whole rest of the SDK: KallistiOS kernel + addons + host utils,
zlib, GLdc, and mkdcdisc. Final image `opencrossing-dc:sdk`.

### Summary

| | Time | Frequency |
|---|---|---|
| Stage 1 (toolchain) | **24 min** | once, ever |
| Stage 2 (KOS/GLdc/mkdcdisc) | **2.5 min** | on any KOS/GLdc bump |
| **Total cold image build** | **~27 min** | — |

Stage 1 is a **one-time cost**. It is a separate Docker layer keyed on
`KOS_SHA` + `TOOLCHAIN_PROFILE`, so it is rebuilt only when the toolchain is
deliberately changed — never during normal development.

### Per-build (what developers actually feel)

Cold full game build ≈ **5–15 min** at `-j4` (§2.1, [UNVERIFIED]); incremental
rebuilds are seconds. Nothing in the daily loop pays the stage-1 cost.

**Budget the toolchain image build as a coffee break, once.** If it must be
faster, use fallback F2 (prebuilt aarch64 tarball, §8) and skip stage 1 entirely.

---

## 7. Bind mount & invocation design

Mirrors `pc/build-armhf-docker.sh` — same shape, one difference: the armhf script
`apt-get install`s its deps on **every** run (it uses a stock `debian:bookworm`).
The DC flow prebakes everything into `opencrossing-dc:sdk`, so per-build runs are
pure compile.

```
dc/Dockerfile            # §5 — builds opencrossing-dc:sdk (once)
dc/build-dc-image.sh     # host-side wrapper around docker build
dc/build-dc-docker.sh    # runs INSIDE the container; builds the game + CDI
harness/dc/smoke.sh      # boots the CDI in Flycast (separate task)
```

Image build (host):

```bash
DOCKER_BUILDKIT=0 docker build --platform linux/arm64 \
  --build-arg JOBS=4 \
  -t opencrossing-dc:sdk -f dc/Dockerfile dc/
```

Game build (host) — note `$PWD` **must** be under `$HOME` (§1.1):

```bash
docker run --rm --platform linux/arm64 \
  -v "$PWD":/work \
  opencrossing-dc:sdk bash /work/dc/build-dc-docker.sh
```

`dc/build-dc-docker.sh` then runs with `KOS_BASE`, `KOS_CC`, `KOS_CFLAGS`,
`KOS_LDFLAGS`, `PATH` (incl. `sh-elf-*` and `$KOS_BASE/utils/build_wrappers`)
already exported by the `dc-env` entrypoint, and produces:

```
/work/dc/build/AnimalCrossing.elf      # 1ST_READ source
/work/dc/build/OpenCrossing.cdi        # gitignored — NEVER commit
```

The entrypoint was verified to export the environment correctly: running
`opencrossing-dc:sdk bash -c 'echo $KOS_BASE'` yields `/opt/toolchains/dc/kos`
with `KOS_SH4_PRECISION=-m4-single`, and `kos-cc`/`kos-c++` are on `PATH`.
[VERIFIED]

CDI step: pass **`-N`** for the Flycast iteration loop and **omit it** for CD-R
burns / read-speed-realistic timing (§5.2 — 1.8 MB/21 ms vs 740 MB/15.6 s).

Design notes:

- Build output goes to `dc/build/` inside the bind mount so artifacts survive the
  container. Add `dc/build/` and `*.cdi`/`*.gdi`/`*.iso` to `.gitignore` — the
  hardware contract in `CLAUDE.md` forbids committing disc images.
- The container runs as root and writes into the mount; virtiofs maps ownership
  back to the host user, so artifacts land owned by `gabe:staff`. **No `--user`
  flag needed.** [VERIFIED — see §4.5.]
- **Do not** mount the user's ISO into the container. Asset extraction is a
  host-side `tools/` step per `PLAN.md` §5; the container only ever sees already-
  extracted assets.
- Keep `--platform linux/arm64` explicit in every invocation. It costs nothing
  and prevents an accidental amd64 pull silently dropping the build into qemu —
  which, per §2, would be slow *and* flaky.
- CI: the image should be pushed to GHCR rather than rebuilt per job. GitHub's
  arm64 runners exist; on amd64 runners the toolchain is native amd64 (no qemu)
  so CI is unaffected by §2. [UNVERIFIED — CI not set up by this pass.]

---
