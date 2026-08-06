# Dreamcast toolchain — component recipes (kos-chain, KOS, GLdc, mkdcdisc)

Upstream state as read from source (`utils/kos-chain`, profiles, float ABI,
thread model), the SH-4 signed-`char` correction, and the build recipe for each
component including the GLdc `GIT_CHANGESET` pinning hazard and the end-to-end
ELF→CDI proof. Read when bumping a component or reproducing the SDK image.
Tag discipline (**[VERIFIED]** = ran it on this host and read the output, **[UNVERIFIED]** = inferred or read from docs) is unchanged from `kb/design-toolchain.md`; do not upgrade an UNVERIFIED claim without re-measuring.

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
kos-ports also carries **`sh4zam`** (SH-4 math library). ❌ **EVALUATED AND
PASSED 2026-08-05 — do not schedule it.** The port already emits FTRV, FIPR and
FSRRA via KOS `dc/fmath.h`; ~~sh4zam's API is `inline` in headers, so from
`src/` its codegen would be decided at `-O0`~~ **[that reason is VOID as of
2026-08-06 — `src/` builds at `-Os` + a 14-TU `-O3` hot list, so header
`inline`s from `src/` do now inline]**; it has no FSQRT; and `kb/perf-dc.md`
§3.7 measured this whole class of change at exactly zero. **The verdict stands
on the other two reasons** — the zero measurement is the load-bearing one — but
if sh4zam is ever reopened, do not cite `-O0` as the argument. Reasons in full:
`kb/closed.md`; the reversal: `kb/state-log.md` 2026-08-06. (This row
previously read "directly relevant to PLAN §3.2's FTRV/FIPR work, evaluate at
M4".)

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
