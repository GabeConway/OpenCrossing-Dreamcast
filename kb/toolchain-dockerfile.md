# Dreamcast toolchain — the SDK Dockerfile and what it was proven to do

The two-stage `dc/Dockerfile` with pinned SHAs, the toolchain identity and
artifacts verified out of the built image, and the mkdcdisc padding measurement
(`-N`: 1.8 MB / 21 ms vs 740 MB / 15.6 s). Read before editing the Dockerfile or
choosing mkdcdisc flags. Tag discipline (**[VERIFIED]** = ran it on this host and read the output, **[UNVERIFIED]** = inferred or read from docs) is unchanged from `kb/design-toolchain.md`; do not upgrade an UNVERIFIED claim without re-measuring.

## 5. The Dockerfile (ready to copy → `dc/Dockerfile`)

Two stages so the expensive toolchain layer is cached independently of the cheap
SDK layer. Pinned SHAs everywhere.

> **[VERIFIED — this Dockerfile was actually built on this host, both stages,
> exit 0.]** Stage 1 → `dc-toolchain:stable` in 24 min; stage 2 →
> `opencrossing-dc:sdk` in 2.5 min. Both images **are already present in the
> local Docker daemon** — the next agent can `docker run opencrossing-dc:sdk`
> immediately without rebuilding anything. See §5.1 for what was proven with it.

```dockerfile
# OpenCrossing-Dreamcast SDK image.
# sh-elf cross toolchain + KallistiOS + GLdc + mkdcdisc, native linux/arm64.
#
# Build once (see §6 for timing):
#   docker build --platform linux/arm64 -t opencrossing-dc:sdk -f dc/Dockerfile dc/
#
# NOTE: this host has no BuildKit. Use the legacy builder:
#   DOCKER_BUILDKIT=0 docker build ...
# and do NOT pass --progress (unsupported, hard-fails).

# ---------------------------------------------------------------- stage 1
FROM debian:bookworm AS toolchain

ARG KOS_SHA=1c6398f9faa55eb397018ef830b3285e6839421b
ARG TOOLCHAIN_PROFILE=stable
ARG JOBS=4
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential make patch gawk file bison flex texinfo \
        libgmp-dev libmpfr-dev libmpc-dev libelf-dev libjpeg-dev libpng-dev \
        wget curl git python3 xz-utils bzip2 ca-certificates \
 && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /opt/toolchains/dc \
 && git clone https://github.com/KallistiOS/KallistiOS.git /opt/toolchains/dc/kos \
 && git -C /opt/toolchains/dc/kos checkout ${KOS_SHA}

# kos-chain reads exactly one config file (`include Makefile.cfg`), so appended
# assignments win over the stock ones. This avoids all sed/quoting traps.
WORKDIR /opt/toolchains/dc/kos/utils/kos-chain
RUN cp Makefile.dreamcast.cfg Makefile.cfg \
 && printf '\n# --- OpenCrossing overrides ---\nenable_objc=0\nenable_objcpp=0\nverbose=0\nmakejobs=%s\n' "$JOBS" >> Makefile.cfg \
 && grep -E '^(enable_|makejobs|verbose|precision_modes|default_precision|thread_model)' Makefile.cfg \
 && make build platform=dreamcast toolchain_profile="$TOOLCHAIN_PROFILE" \
 && make distclean

# ---------------------------------------------------------------- stage 2
FROM toolchain AS sdk

ARG KOS_PORTS_SHA=f4faacc42faaf552625777b7709e871a827e1055
ARG GLDC_SHA=a1cd80a8dbc1923237f3418529f43b4851da85af
ARG MKDCDISC_SHA=3c2ef63a9e0d68afbe21ca5b2b294aecf7392e8f
ARG JOBS=4
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake meson ninja-build pkg-config libisofs-dev \
 && rm -rf /var/lib/apt/lists/*

> **Note on the `-O2` in `KOS_CFLAGS` below (clarified 2026-08-06).** That
> `-O2` builds **KOS, newlib and kos-ports**, and always has. It was never the
> level `src/` was compiled at, so it was not covered by — and did not violate
> — the old `-O0` directive. Since the reversal, `src/` builds at `-Os` + a
> 14-TU `-O3` hot list and `dc/src` at `-O3`; the game build assembles its own
> `-O` and appends it *after* `$KOS_CFLAGS`, so this line still does not decide
> it. Do not "fix" this line to match the game's level: changing it means
> rebuilding the SDK image (~27 min cold) and re-validating the prebuilt
> libraries. See `dc/opt-lists.mk`, `BUILDING-DC.md`, and `kb/state-log.md`
> 2026-08-06.

# environ.sh — KOS_*FLAGS must be "" before environ_base.sh, which appends.
RUN printf '%s\n' \
  'export KOS_ARCH="dreamcast"' \
  'export KOS_BASE="/opt/toolchains/dc/kos"' \
  'export KOS_PORTS="/opt/toolchains/dc/kos-ports"' \
  'export KOS_CC_BASE="/opt/toolchains/dc/sh-elf"' \
  'export KOS_CC_PREFIX="sh-elf"' \
  'export DC_TOOLS_BASE="/opt/toolchains/dc/bin"' \
  'export KOS_CMAKE_TOOLCHAIN="${KOS_BASE}/utils/cmake/kallistios.toolchain.cmake"' \
  'export KOS_GENROMFS="${KOS_BASE}/utils/genromfs/genromfs"' \
  'export KOS_MAKE="make"' \
  'export KOS_INC_PATHS=""' \
  'export KOS_CFLAGS=""' \
  'export KOS_CPPFLAGS=""' \
  'export KOS_LDFLAGS=""' \
  'export KOS_AFLAGS=""' \
  'export KOS_SH4_PRECISION="-m4-single"' \
  'export KOS_CFLAGS="${KOS_CFLAGS} -O2 -fno-PIC -fno-PIE -fomit-frame-pointer"' \
  '. ${KOS_BASE}/environ_base.sh' \
  > /opt/toolchains/dc/kos/environ.sh

# KallistiOS proper: kernel + addons + host utils (pvrtex, wav2adpcm, vqenc...).
# Single-quoted so the outer sh passes it through; bash expands $KOS_BASE (from
# environ.sh) and $JOBS (inherited from the ARG).
RUN bash -c '. /opt/toolchains/dc/kos/environ.sh && make -C "$KOS_BASE" -j"$JOBS"'

# kos-ports: zlib, then GLdc (port name is "libGL"); GIT_CHANGESET pins GLdc.
RUN git clone https://github.com/KallistiOS/kos-ports.git /opt/toolchains/dc/kos-ports \
 && git -C /opt/toolchains/dc/kos-ports checkout "$KOS_PORTS_SHA" \
 && bash -c '. /opt/toolchains/dc/kos/environ.sh \
      && make -C "$KOS_PORTS/zlib"  install clean \
      && make -C "$KOS_PORTS/libGL" install clean GIT_CHANGESET="$GLDC_SHA"'

# mkdcdisc (host tool, ELF -> CDI)
RUN git clone https://gitlab.com/simulant/mkdcdisc.git /tmp/mkdcdisc \
 && git -C /tmp/mkdcdisc checkout "$MKDCDISC_SHA" \
 && meson setup /tmp/mkdcdisc/builddir /tmp/mkdcdisc \
 && meson compile -C /tmp/mkdcdisc/builddir \
 && install -m 0755 /tmp/mkdcdisc/builddir/mkdcdisc /usr/local/bin/mkdcdisc \
 && rm -rf /tmp/mkdcdisc

# Entrypoint sources the KOS environment, then execs the command.
RUN printf '#!/bin/bash\n. /opt/toolchains/dc/kos/environ.sh\nexec "$@"\n' \
      > /usr/local/bin/dc-env && chmod +x /usr/local/bin/dc-env

WORKDIR /work
ENTRYPOINT ["/usr/local/bin/dc-env"]
CMD ["/bin/bash"]
```

Pinned revisions, all [VERIFIED] as existing at time of writing:

| Component | SHA / version | Date |
|---|---|---|
| KallistiOS | `1c6398f9faa55eb397018ef830b3285e6839421b` | 2026-07-24 |
| kos-ports | `f4faacc42faaf552625777b7709e871a827e1055` | 2026-06-24 |
| GLdc | `a1cd80a8dbc1923237f3418529f43b4851da85af` | 2026-07-17 |
| mkdcdisc | `3c2ef63a9e0d68afbe21ca5b2b294aecf7392e8f` | 2026-07-31 |
| binutils / GCC / newlib | 2.45.1 / 15.2.0 / 4.6.0.20260123 | via `stable` profile |

### 5.1 What the built image was proven to do [ALL VERIFIED]

Toolchain identity, straight out of `opencrossing-dc:sdk`:

```
sh-elf-gcc (GCC) 15.2.0        sh-elf-g++ (GCC) 15.2.0
--with-multilib-list=m4-single,m4-single-only   --with-cpu=m4-single
--enable-threads=kos           Thread model: kos
-print-multi-lib -> ".;"  and  "m4-single-only;@m4-single-only"
-dM -E defines __SH4_SINGLE__   (i.e. m4-single default -> real 64-bit doubles)
default char signedness: SIGNED    <-- confirms §3.1 on GCC 15.2.0
KOS_SH4_PRECISION=-m4-single
```

Artifacts present and non-empty:

| Artifact | Result |
|---|---|
| `$KOS_BASE/lib/dreamcast/libkallisti.a` | 5,786,396 bytes |
| `$KOS_PORTS/lib/libGL.a`, `libGLU.a` | present (GLdc) |
| `$KOS_PORTS/lib/libz.a` | present |
| `$KOS_PORTS/include/GL/` | `gl.h glext.h glkos.h glu.h` |
| KOS host utils | `pvrtex`, `wav2adpcm`, `vqenc`, `genromfs`, `bin2c` all built |
| `/usr/local/bin/mkdcdisc` | v0.0.2, runs |

Compile/link smoke tests inside the image:

```
kos-cc -fsigned-char -o hello.elf hello.c -lGL     # C + GLdc
   text 219660   data 7560   bss 27212
kos-c++ -o tcpp.elf t.cpp                          # C++ + libstdc++ (std::vector)
   text 270637   data 6644   bss 22564
mkdcdisc -q -e hello.elf -d data -o OpenCrossing.cdi
   Binary start: 0x8c010000   ->  wrote a bootable CDI
```

C, C++/libstdc++, GLdc linking, and CDI generation all work. **The M0 toolchain
gate is met.**

### 5.2 mkdcdisc padding — matters a lot for iteration speed [VERIFIED]

Same ELF, two invocations:

| Invocation | Output size | Time |
|---|---|---|
| `mkdcdisc -e … -o out.cdi` (default) | **740,083,145 B** (740 MB) | **15.6 s** |
| `mkdcdisc -N -e … -o out.cdi` (`--no-padding`) | **1,783,337 B** (1.8 MB) | **0.021 s** |

A **415× size and 740× time** difference. The default pads the data track to the
full disc *deliberately* — mkdcdisc's help says padding is "to optimize loading
speed", i.e. it pushes content toward the outer tracks. That is exactly the
outer-track placement `PLAN.md` §5 calls for on CD-R.

So use both, for different purposes:

- **`-N` for the Flycast smoke loop** (`harness/dc/smoke.sh`). 1.8 MB and
  instant; 740 MB per iteration would be untenable.
- **Default padding for CD-R burns and any read-speed-realistic timing.**
  Measuring streaming/acre-load performance against an unpadded image would give
  optimistic numbers.

---
