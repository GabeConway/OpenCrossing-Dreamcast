# OpenCrossing-Dreamcast SDK image.
# sh-elf cross toolchain + KallistiOS + GLdc + mkdcdisc, native linux/arm64.
#
# Build once (~27 min cold; see kb/toolchain.md):
#   dc/build-dc-image.sh
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
