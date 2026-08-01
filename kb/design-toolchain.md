# Dreamcast toolchain — design & measurements (M0)

Written 2026-08-01 by the toolchain recon pass. Everything below is tagged
**[VERIFIED]** (I ran it on this host and read the output) or **[UNVERIFIED]**
(inferred, extrapolated, or read from docs but not executed). Do not upgrade an
UNVERIFIED claim without re-measuring.

---

## 0. Decision (TL;DR)

**Build the toolchain from source with KOS's own `kos-chain`, natively for
linux/arm64, inside a Debian bookworm container.** Bake toolchain + KallistiOS +
GLdc + mkdcdisc into one image `opencrossing-dc:sdk`; bind-mount the repo at
`/work` and invoke `dc/build-dc-docker.sh`, exactly mirroring the base repo's
`pc/build-armhf-docker.sh` pattern.

> **This is not a proposal — it was executed.** The Dockerfile in §5 was built on
> this host (stage 1: 24 min, stage 2: 2.5 min, both exit 0). The resulting image
> compiles and links C, C++/libstdc++ and GLdc for sh-elf and produces a bootable
> CDI (§5.1). **Both images already exist in the local Docker daemon**, so the
> next agent can start using `opencrossing-dc:sdk` immediately.
>
> Remaining risk is not "does the toolchain work" but "does the *decomp* build
> with GCC 15.2.0" — a separate question, gated at M1.

Rejected alternatives, with the measurement that killed each:

| Option | Verdict | Why |
|---|---|---|
| `einsteinx2/dcdev-kos-toolchain` (arm64) | **fallback only** | Native arm64 and *works today*, but image built **2021-03-15**: GCC 9.3, KOS master-of-2021, `m4-single-only` ABI, no mkdcdisc. [VERIFIED] |
| `nold360/kallistios-sdk` | **rejected** | Publishes **amd64 only** — no arm64 manifest. [VERIFIED] |
| Any amd64 image under qemu | **rejected** | **17–23× slower** *and* throws **nondeterministic ICEs**. [VERIFIED] |
| Prebuilt `drpaneas` linux-aarch64 tarball | **fallback** | Real, GCC 15.1.0 + KOS 2.2.1, 470 MB. Third-party, pinned to a KOS *tag*. [VERIFIED it exists; UNVERIFIED that it runs] |

---

## 1. Host environment — measured, and it is not what the brief said

| Fact | Value | Status |
|---|---|---|
| Host | macOS, Apple M4 arm64, 10 cores, 24 GB | given |
| Docker | client 29.6.1 (darwin/arm64), server 29.5.2 (**linux/arm64**), context `colima` | [VERIFIED] |
| **colima VM CPUs** | **4** (not 10) | [VERIFIED] `docker info` + `colima.yaml` |
| **colima VM RAM** | **8 GiB** (not 24) | [VERIFIED] |
| colima disk free | 81 GB in VM, 87 GB on host | [VERIFIED] |
| mountType | virtiofs | [VERIFIED] |
| BuildKit | **absent** — `docker build --progress` fails with `unknown flag`; legacy builder only | [VERIFIED] |
| binfmt/qemu for amd64 | present and working (`tonistiigi/binfmt` image installed) | [VERIFIED] |

### 1.1 Bind-mount constraint (this bites, plan around it)

**Only paths under `/Users/gabe` are shared into the colima VM.** [VERIFIED]

`/private/tmp/...` (the agent scratchpad) mounts as an **empty directory** — Docker
silently creates the mountpoint inside the VM and you get nothing. This is a
silent-wrong-answer failure, not an error. Confirmed by mounting the scratchpad
(empty) vs mounting the repo at `/Users/gabe/Documents/GitHub/OpenCrossing-Dreamcast`
(full contents visible). [VERIFIED]

Consequence: **every bind mount in every DC build script must be under `$HOME`.**
The repo itself qualifies, so the base-repo pattern works unchanged.

### 1.2 Recommended host change (not applied — needs the user)

colima is provisioned with 4 of 10 cores and 8 of 24 GB. The toolchain build and
the ~4,800-file game build are both `make -j` bound. Recommend:

```bash
colima stop && colima start --cpu 8 --memory 16 --disk 100
```

**Not done by this pass** — it would kill the in-flight build and any other
agent's containers. All build times below are measured at **4 cores** and are
therefore an upper bound. [VERIFIED that colima is at 4 CPUs; UNVERIFIED what
8 cores would give — expect roughly 0.55–0.65× wall clock, not 0.5×.]

---

## 2. Why not qemu: the measurement

Same image (`einsteinx2/dcdev-kos-toolchain`, which ships **both** arm64 and
amd64 manifests — a clean A/B), same corpus, same host. Corpus = 100 synthetic
self-contained C TUs, mean 13.9 KB, compiled `-O2 -fsigned-char`.

| Workload | arm64 native | amd64 under qemu | ratio |
|---|---|---|---|
| 20 × trivial `hello.c`, serial | **0.184 s** (9.2 ms/file) | **4.253 s** (213 ms/file) | **23.1×** |
| 100 × 13.9 KB TU, `-O2`, serial | **2.783 s** (27.8 ms/file) | *did not complete* — ICE | — |
| 10 × 13.9 KB TU, `-O2`, serial | — (see row above: 27.8 ms/file) | **4.66 s** mean of 3 runs (466 ms/file) | **16.8×** |
| 100 × 13.9 KB TU, `-O2`, `-j4` | **0.741 s** | not run | — |

[VERIFIED — every bolded number is a real timing from this host. The qemu
100-TU serial run is absent because it *crashed*, which is the point; the 10-TU
qemu row exists so there is a like-for-like per-file rate to compare against.]

**qemu is not merely slow, it is unsound.** The 100-TU serial run under qemu died
with:

```
sh-elf-gcc: internal compiler error: Segmentation fault signal terminated program cc1
```

Re-running 10 TUs three times: **1 failure in 30 compiles (3.3%), nondeterministic**
— the same file compiled fine on the next attempt. [VERIFIED] The identical corpus
compiled 100/100 natively on arm64. A ~3% random per-TU failure rate across 4,824
files makes a build statistically impossible to complete, and worse, would produce
*intermittent* failures that look like source bugs. This alone disqualifies qemu
regardless of speed.

### 2.1 Extrapolation to the real tree

Repo has **4,824** `.c/.cpp/.s` files under `src/` (4,765 `.c`, 809,797 LOC),
mean 6.95 KB, median 3.1 KB, p90 11.7 KB, max 3.69 MB
(`src/data/field/bg/acre/bg_data.c`). [VERIFIED — counted]

| Path | Serial | `-j4` |
|---|---|---|
| arm64 native | 4824 × 27.8 ms ≈ **134 s** | ≈ **36 s** |
| amd64 qemu | 4824 × 466 ms ≈ **37.5 min** | ≈ 10 min (plus ~160 random ICEs) |

[UNVERIFIED extrapolation.] Caveats, both directions: my corpus averages 13.9 KB
(2× the real mean, so *pessimistic* per file) but is fully self-contained, whereas
real decomp TUs pull the whole `include/` tree (so *optimistic* on preprocessing),
and `src/data/` contains multi-MB generated tables that will dominate. **Realistic
expectation for a full cold game build on arm64 at `-j4`: 5–15 minutes.**
[UNVERIFIED — measure at M1 and replace this line with the real number.]

The load-bearing, verified number is the **17–23× ratio plus the ICEs**. That is
what makes the decision, and it is not close.

---

## 3. Current upstream state (read from source, 2026-08-01)

All [VERIFIED] by cloning and reading the files.

- **`utils/dc-chain` no longer exists. It is `utils/kos-chain`.** Any doc or
  script referencing `utils/dc-chain` is stale. KOS master HEAD =
  `1c6398f9faa55eb397018ef830b3285e6839421b` (2026-07-24, *"kos-chain: Fix
  parallel libgcc build race in SH GCC patches"* — note: a very recent fix to
  parallel builds, which is exactly the code path we exercise with `makejobs`).
- **Stable profile** (`profiles/dreamcast/stable.mk`): binutils **2.45.1**,
  GCC **15.2.0**, newlib **4.6.0.20260123**. Other profiles available:
  `legacy` (GCC 13.2.0), `13.4.0`, `14.4.0`, `15.3.0`, `16.1.0`, plus `-dev`
  git profiles up to `17.0.0-exp`.
- **Float ABI**: `precision_modes=m4-single,m4-single-only` (multilib, both
  built), `default_precision=m4-single`. The config file states verbatim:
  *"m4-single is the default as of KOS 2.2.0 to increase compatibility with newer
  libraries which require 64-bit doubles."* This confirms the brief. Under
  `m4-single-only`, **64-bit doubles are truncated to 32-bit floats** — a silent
  correctness hazard for decomp code that uses `double`. **Use `m4-single`.**
- **Thread model**: `thread_model=kos` (KOS patches GCC to add a `kos` thread
  model). `use_kos_patches` and `auto_fixup_newlib` default on; leave them on.
- **Language knobs**: `enable_cpp=1`, `enable_objc=1`, `enable_objcpp=1` by
  default. We need C++ (the decomp has `jsyswrap.cpp`) but **not** Obj-C or
  Obj-C++ — turning those off is free build time.
- **`makejobs`** is empty by default (= auto-detect CPU threads). Set explicitly.
- **`make distclean`** removes only download/build dirs, **not** the installed
  toolchain in `/opt/toolchains/dc/sh-elf`. Safe to run to shrink the image.
  [VERIFIED by reading `scripts/clean.mk`.]
- Upstream ships its own `utils/kos-chain/docker/Dockerfile` (Alpine, two-stage,
  `FROM scratch` flatten). Ours is Debian-based instead, to match the base repo's
  `debian:bookworm` convention and because we also need `libisofs-dev`/`meson`
  from Debian for mkdcdisc.

### 3.1 kb correction: SH-4 `char` is SIGNED by default

`kb/base-repo-map.md` says *"`-fsigned-char` required (PPC chars signed; ARM and
SH-4 GCC default unsigned)"*. **The SH-4 half of that is wrong.** Tested on
`sh-elf-gcc`:

```c
int a[(char)-1 < 0 ? 1 : -1];   /* compiles only if char is signed */
```

compiles cleanly, and `-dM -E` emits **no** `__CHAR_UNSIGNED__`. [VERIFIED on
GCC 9.3.0/sh-elf; UNVERIFIED on GCC 15.2.0 — recheck once the stable toolchain
is built, though `DEFAULT_SIGNED_CHAR` for SH is architectural and very unlikely
to have changed.]

Keep passing `-fsigned-char` anyway — it is harmless, explicit, and matches the
base repo — but do **not** treat it as load-bearing on SH-4 the way it is on ARM.
Someone should fix that sentence in `kb/base-repo-map.md` (not edited by this
pass — not my assigned file).

---

## 4. Component recipes

### 4.1 sh-elf toolchain — `kos-chain`

```bash
cd $KOS_BASE/utils/kos-chain
cp Makefile.dreamcast.cfg Makefile.cfg
# edits: enable_objc=0, enable_objcpp=0, makejobs=<N>, verbose=0
make build platform=dreamcast toolchain_profile=stable
make distclean
```

Installs to `/opt/toolchains/dc/sh-elf`. The `arm-eabi` (AICA ARM7) toolchain is
a **separate** `platform=aica` build and is **optional**. **Skip it** — it saves a
whole second toolchain build, and `PLAN.md` §3.4 explicitly plans no CPU work on
the ARM7.

**Skipping is safe, and KOS handles it explicitly** [VERIFIED by reading
`kernel/arch/dreamcast/sound/arm/Makefile`]:

```make
ARM_CC_IS_AVAILABLE=0
ifdef DC_ARM_CC
  ifneq ("$(wildcard $(DC_ARM_CC))", "")
    ARM_CC_IS_AVAILABLE=1
  endif
endif
ifeq ($(ARM_CC_IS_AVAILABLE), 1)
stream.drv: prog.elf            # build the AICA driver from source
else
stream.drv: stream.drv.prebuilt # otherwise use the checked-in binary
endif
```

`stream.drv.prebuilt` is present in the tree, so the KOS build succeeds with no
ARM compiler and simply uses the shipped driver. Add `platform=aica` later only
if a custom AICA driver becomes necessary (PLAN §3.4 Stage B). [Recipe VERIFIED
from source.]

Debian bookworm build deps [VERIFIED — this exact list installed cleanly and
kos-chain got through binutils + into GCC pass1 with it]:

```
build-essential make patch gawk file bison flex texinfo
libgmp-dev libmpfr-dev libmpc-dev libelf-dev libjpeg-dev libpng-dev
wget curl git python3 xz-utils bzip2 ca-certificates
```

### 4.2 KallistiOS

`make` at `$KOS_BASE` builds `SUBDIRS = utils kernel addons`. Note `utils`
includes the host tools we actually want: **`pvrtex`** (PLAN §3.3 texture
pipeline), **`wav2adpcm`** and **`vqenc`** (PLAN §3.4 audio), plus `bin2c`,
`genromfs`, `makeip`, `scramble`, `dcbumpgen`, `kmgenc`. [VERIFIED from
`Makefile` / `utils/Makefile`.] Requires `environ.sh` sourced first — the root
Makefile hard-errors without `KOS_BASE`.

`environ.sh` must set the `KOS_*FLAGS` vars to `""` **before** sourcing
`environ_base.sh`, because that script *appends* to them. Template below is
derived from `doc/environ.sh.sample`. [VERIFIED by reading both files.]

`environ_dreamcast.sh` appends these to `KOS_CFLAGS`:
`-m4-single -ml -mfsrra -mfsca -ffunction-sections -fdata-sections
-matomic-model=soft-gusa -ftls-model=local-exec`, and to `KOS_LDFLAGS`:
`-m4-single -ml -Wl,--gc-sections` with
`KOS_LD_SCRIPT=-T$KOS_BASE/utils/ldscripts/shlelf.xc`. [VERIFIED]

### 4.3 GLdc — via kos-ports, and it is called `libGL`

**There is no `GLdc` directory in kos-ports. The port is named `libGL`.**
[VERIFIED] `kos-ports/libGL/Makefile`:

```
PORTNAME = libGL ; PORTVERSION = 1.1.1 ; PORT_BUILD = cmake
GIT_REPOSITORY = https://gitlab.com/simulant/GLdc.git
TARGET = libGL.a libGLU.a
INSTALLED_HDRS = include/GL/{gl,glext,glkos,glu}.h  (installed under GL/)
CMAKE_ARGS = -DCMAKE_BUILD_TYPE=Release
```

(Do **not** confuse it with `kos-ports/libKGL`, which is the older, unrelated GL.)

Install: `make -C $KOS_PORTS/libGL install clean`.

**Reproducibility hazard [VERIFIED]:** `libGL` sets neither `GIT_TAG` nor
`GIT_BRANCH`, so `scripts/download.mk` does a plain `git clone` of GLdc
**master** — an unpinned moving target. `download.mk` *does* honour
`GIT_CHANGESET` (`git reset --hard $GIT_CHANGESET` after clone) when `GIT_TAG` is
empty. **Always pass `GIT_CHANGESET=<sha>`.** Current GLdc master =
`a1cd80a8dbc1923237f3418529f43b4851da85af` (2026-07-17, actively maintained).

Manual (non-kos-ports) build also works and is the escape hatch if the port
misbehaves — GLdc is CMake with its own KOS toolchain file
[VERIFIED from GLdc's README/`toolchains/Dreamcast.cmake`]:

```bash
cmake -DCMAKE_TOOLCHAIN_FILE=../toolchains/Dreamcast.cmake -G "Unix Makefiles" \
      -DCMAKE_BUILD_TYPE=Release -DBUILD_SAMPLES=OFF -DBUILD_TESTS=OFF ..
```

kos-ports `config.mk` sets `CHECK_PRECISION = true`, so ports validate against
`KOS_SH4_PRECISION` — another reason that variable must be set to `-m4-single`
consistently. [VERIFIED]

Also install **`zlib`** from kos-ports (base port uses it). Worth knowing:
kos-ports also carries **`sh4zam`** (SH-4 math library) — directly relevant to
PLAN §3.2's FTRV/FIPR work, evaluate at M4. [VERIFIED it exists in the tree;
UNVERIFIED what it contains.]

### 4.4 mkdcdisc — **built and run successfully on arm64 Debian bookworm**

[VERIFIED end-to-end: cloned, built, ran `--help`.]

```
apt: git meson ninja-build build-essential pkg-config libisofs-dev
meson setup builddir && meson compile -C builddir
```

Debian bookworm's `meson 1.0.1` / `ninja 1.11.1` / `libisofs-dev 1.5.4-1` are
sufficient — **no backports or pip needed**. HEAD =
`3c2ef63a9e0d68afbe21ca5b2b294aecf7392e8f` (2026-07-31, very active; GDI support
landed the day before this was written). Version string `v0.0.2`.

Outputs `.cdi` (default), `.mds/.mdf`, `.nrg`, and `.gdi`. **IP.BIN bootstraps
and the MR license logo are embedded in the binary** — no external `makeip` /
`scramble` / `img4dc` step, and no extra assets to vendor. [VERIFIED from
`BUILDING.md` + `meson.build`.] Basic invocation is
`mkdcdisc -e <elf> -o <out.cdi>` (plus `-d <dir>` style options for the data
tree — read `--help` at M0). Use `-q/--no-banner` for machine-readable output.

### 4.5 End-to-end pipeline proof [VERIFIED]

Before trusting any of the above, I ran the whole chain on this host, arm64
native, no qemu: **KOS source → `hello.elf` → bootable `.cdi`.**

```
kos-cc -o hello.elf hello.c                      # in the arm64 KOS image
mkdcdisc -e hello.elf -d <datadir> -o hello.cdi  # in an arm64 Debian container
```

mkdcdisc output:

```
Loaded 2536348 bytes from ELF
Binary start: 0x8c010000   Binary end: 0x8c07b6bc   Binary size: 0x6b6bc
Generating data track...    Done.
Wrote hello.cdi            (1,991,241 bytes)
```

Two things worth carrying forward:

- **`Binary start: 0x8c010000`** — confirms the KOS load address. `PLAN.md` §11.6
  flags re-deriving emu64's seg2k0 pointer heuristic against KOS's `0x8C000000`
  RAM base; this is that base, observed. [VERIFIED]
- **Bind-mount write-back works and ownership is correct.** The container (root)
  wrote `hello.elf` into a `$HOME` bind mount and the host sees it owned by
  `gabe:staff`, not root. So §7's `--user` workaround is **not** needed.
  [VERIFIED — this resolves what was an open question.]

The `.cdi` was produced outside the repo and deleted. **Never commit disc
images** (`CLAUDE.md` hardware contract).

---

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

## 8. Fallback plan

Ordered. Each is independently sufficient to unblock M0.

**F1 — `einsteinx2/dcdev-kos-toolchain:latest`, arm64. Works today; verified.**

```bash
docker pull --platform linux/arm64 einsteinx2/dcdev-kos-toolchain:latest
docker run --rm --platform linux/arm64 -v "$PWD":/src \
  einsteinx2/dcdev-kos-toolchain:latest    # entrypoint sets KOS env
```

I compiled and linked a KOS hello-world in it: [VERIFIED]

```
. /opt/toolchains/dc/kos/environ.sh
kos-cc -o hello.elf hello.c
  ->  text 242208  data 12476  bss 185148   (hello.elf, 2.5 MB unstripped)
```

`libkallisti.a` (5.1 MB) and `kos-ports/lib/libGL.a` (GLdc) are both present and
prebuilt. Caveats, all [VERIFIED]: image dated **2021-03-15**; GCC **9.3.0**;
`__SH4_SINGLE_ONLY__` is defined, i.e. **`m4-single-only` — doubles truncate to
float**; `KOS_SH4_PRECISION` is unset (the variable postdates the image); no
mkdcdisc (but `cdi4dc`/`makeip`/`scramble` are there, and mkdcdisc builds
separately in a plain Debian arm64 container in ~1 minute, per §4.4). Good enough
to prove `harness/dc/smoke.sh` end to end; **not** good enough to build the game
on (5-year-old KOS, wrong float ABI).

**F2 — prebuilt `drpaneas/dreamcast-toolchain-builds`, `linux-aarch64`.**
Release `gcc15.1.0-kos2.2.1` (2025-12-29) publishes
`dreamcast-toolchain-gcc15.1.0-kos2.2.1-linux-aarch64.tar.gz` (470 MB, with
`.sha256`). GCC 15.1.0 + KOS **v2.2.1**. [VERIFIED the asset exists via the
GitHub API; **UNVERIFIED** that it unpacks/runs — I did not download it.] Use if
the from-source build proves unreliable: `ADD` the tarball into a Debian arm64
image and skip stage 1 entirely. Trade-off: third-party binaries, KOS pinned to a
2024-era tag rather than master, and no control over profile knobs.

**F3 — swap the toolchain profile.** If GCC 15.2.0 miscompiles the decomp (see
`PLAN.md` §3.2 — this codebase has a documented history of optimizer breakage),
change one ARG: `TOOLCHAIN_PROFILE=legacy` (GCC 13.2.0) or `14.4.0`. Note sm64-dc
shipped on GCC 14.1 per `kb/research-dreamcast.md`, so `14.4.0` is the
best-precedented middle option.

**F4 — Alpine instead of Debian.** Upstream's own
`utils/kos-chain/docker/Dockerfile` is Alpine and is the configuration upstream
CI exercises. If the Debian host toolchain (GCC 12) fails to build GCC 15.2.0,
switch the base image and follow `doc/alpine.md`. mkdcdisc would then need
`libisofs` from Alpine (`apk add libisofs-dev meson`) — [UNVERIFIED that Alpine
packages it].

**F5 — reduce parallelism.** kos-chain's own docs warn that multiple jobs "may
cause issues in certain environments"; KOS master's most recent commit is
literally a fix for a **parallel libgcc build race**. If stage 1 fails
nondeterministically, set `--build-arg JOBS=1` and accept the wall clock.

**Never a fallback: running an amd64 image under qemu.** See §2.

---

## 9. Open items for the next agent

1. Commit the §5 Dockerfile as `dc/Dockerfile` and write
   `dc/build-dc-docker.sh` per §7. The images already exist locally; committing
   the Dockerfile is what makes them reproducible.
2. Add `dc/build/`, `*.cdi`, `*.gdi`, `*.mds`, `*.mdf`, `*.nrg` to `.gitignore`
   before anything writes a disc image into the tree.
3. Fix the wrong sentence in `kb/base-repo-map.md` — SH-4 GCC `char` is
   **signed** by default, confirmed on GCC 15.2.0 (§3.1, §5.1). Not edited here
   because it is not this pass's assigned file.
4. Measure a real full-tree compile (4,824 files) and replace the §2.1
   extrapolation with the true number.
5. Ask the user to re-provision colima at 8 CPUs / 16 GB (§1.2) — every build
   time in this document was measured at 4 cores.
6. Evaluate kos-ports `sh4zam` against `PLAN.md` §3.2's hand-rolled FTRV/FIPR
   plan before writing SH-4 math by hand.
7. Decide the GCC-15.2.0-vs-decomp question at M1. `PLAN.md` §3.2 documents this
   codebase's history of optimizer breakage; fallback F3 (profile `14.4.0`,
   matching sm64-dc's GCC 14) is one ARG change away.

---

## 10. Reproducing the measurements

So a later pass can audit or re-run any number in §2. Scratch dir must be under
`$HOME` (§1.1).

```bash
# corpus: 100 self-contained C TUs, mean 13.9 KB (generator was throwaway;
# any similar corpus reproduces the ratio — it is not sensitive to the details)
mkdir -p ~/.cache/dcbench/src

# native arm64
docker run --rm --platform linux/arm64 -v ~/.cache/dcbench:/bench \
  --entrypoint /bin/sh einsteinx2/dcdev-kos-toolchain:latest -c '
    G=/opt/toolchains/dc/sh-elf/bin/sh-elf-gcc; cd /bench
    time (for f in src/tu_*.c; do $G -O2 -fsigned-char -c $f -o /tmp/x.o; done)'

# same image, amd64 manifest, under qemu — expect ~17x and sporadic ICEs
docker run --rm --platform linux/amd64 -v ~/.cache/dcbench:/bench \
  --entrypoint /bin/sh einsteinx2/dcdev-kos-toolchain:latest -c '...same...'
```

Manifest checks that decided §0:

```bash
docker manifest inspect einsteinx2/dcdev-kos-toolchain:latest  # -> amd64 AND arm64
docker manifest inspect nold360/kallistios-sdk:latest          # -> amd64 only
```

Note `docker manifest inspect nold360/kallistios-sdk:latest` returns a bare
single-arch manifest with no `platform` block; use `-v` to see
`"architecture": "amd64"`. `ghcr.io/nold360/*` and `ghcr.io/kallistios/*` both
return `denied` — those namespaces do not publish public images. [VERIFIED]
