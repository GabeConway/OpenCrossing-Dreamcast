# Building OpenCrossing for the Sega Dreamcast

Everything happens inside one Docker image, `opencrossing-dc:sdk`, which
carries sh-elf-gcc 15.2.0, KallistiOS, GLdc (`-lGL`), zlib and `mkdcdisc`.
The host needs nothing but Docker (colima on this machine).

> `pc/` is the Linux/SDL reference port. It is **not** the Dreamcast build.
> Do not run CMake for Dreamcast — the DC build is a plain GNU makefile.

---

## Quick start

```bash
cd /Users/gabe/Documents/GitHub/OpenCrossing-Dreamcast

bash dc/build-dc-image.sh        # once, ~27 min. Skip if the image exists.
bash dc/build-dc.sh              # ELF + CDI
```

Artifacts land in `dc/build/` (gitignored — **never** commit a disc image):

| File | What it is |
|---|---|
| `dc/build/AnimalCrossing.elf` | unstripped ELF. Keep it: `sh-elf-addr2line -e` on it turns a crash PC into file:line. |
| `dc/build/AnimalCrossing.map` | link map |
| `dc/build/OpenCrossing.cdi` | the disc image |
| `dc/build/obj/**` | objects + `.d` files, mirroring the source tree |

---

## The three entry points

| File | Runs where | Job |
|---|---|---|
| `dc/build-dc.sh` | host | `docker run` wrapper. Checks the image exists, forwards env, mounts the repo at `/work`. |
| `dc/build-dc-docker.sh` | container | drives `make`, then `mkdcdisc`. |
| `dc/Makefile` | container | the actual build. |

You can also drive `make` directly:

```bash
docker run --rm --platform linux/arm64 \
  -v /Users/gabe/Documents/GitHub/OpenCrossing-Dreamcast:/work \
  opencrossing-dc:sdk bash -c 'make -C /work/dc -j4 objs'
```

`--platform linux/arm64` is not optional. Without it an accidental amd64 pull
drops the whole build into qemu — slow and flaky
(`kb/design-toolchain.md` §2).

---

## Make targets

| Target | Effect |
|---|---|
| `make objs` | compile every TU, **do not link**. This is the milestone-1 signal. |
| `make all` (default) | `objs` + link → `dc/build/AnimalCrossing.elf` |
| `make clean` | `rm -rf dc/build` |
| `make count` | print TU counts |
| `make sources` | dump the computed source list — use this when debugging the exclusion filters |

`make count` should print **3917 TUs**:

| | count |
|---|---|
| decomp `.c` (after the 35 inherited PC filters) | 3854 |
| decomp `.cpp` | 46 |
| `src/static/dolphin/pad/Padclamp.c` (added back) | 1 |
| `pc/src` files reused verbatim | 3 |
| `dc/src/*.c` | 13 |

3854 + 46 = 3900 is exactly the number `kb/design-shelf-hazards.md` §0
measured as compiling for sh-elf, so a different number means a filter broke.

Parallel and incremental builds both work: `make -j8`, and re-running `make`
after touching one file rebuilds only what depends on it (GCC `-MMD -MP`
dependency files live next to the objects). Measured on this host, inside
colima (4 cores), `-j4`:

| | time |
|---|---|
| clean `make all` (3917 TUs + link + CDI) | **97 s** |
| `touch include/m_play.h` → `make objs` (2604 TUs) | 78 s |
| no-op `make objs` | < 2 s |

---

## Environment knobs

| Var | Default | Meaning |
|---|---|---|
| `JOBS` | `4` | `make -j` level. The colima VM has 4 cores. |
| `DC_TARGET` | `all` | pass `objs` for a compile-only run |
| `DC_CDI_PAD` | `0` | `1` → padded 740 MB CDI (see below) |
| `DECOMP_OPT` | `-O0` | optimization level for decomp game code |
| `DC_OPT` | `-O2` | optimization level for `dc/src` platform code |
| `V` | unset | `V=1` echoes full compiler command lines |

```bash
DC_TARGET=objs bash dc/build-dc.sh     # compile-only
DC_CDI_PAD=1   bash dc/build-dc.sh     # CD-R burn image
JOBS=8         bash dc/build-dc.sh
DECOMP_OPT=-O2 bash dc/build-dc.sh     # see the warning below
```

---

## Padded vs unpadded CDI

Measured (`kb/design-toolchain.md` §5.2):

| | size | time |
|---|---|---|
| `mkdcdisc -N` (default here) | 1,783,337 B | 0.021 s |
| `mkdcdisc` (padded) | 740,083,145 B | 15.6 s |

The default is `-N`, because 740 MB per iteration would make the Flycast loop
untenable. The padding is not waste — it pushes content toward the outer
tracks, which is what a real CD-R wants. **Use `DC_CDI_PAD=1` for anything
you burn, and for any timing run that has to be read-speed-realistic.**
Streaming numbers measured against an unpadded image are optimistic.

---

## Optimization level — `-O0`, and this is not a tunable

**Project directive (2026-08-01): raising `DECOMP_OPT` is banned.** Verbatim:
*"the optimizations cause problems and we cant use them without the port being
broken."* This is a decision, not a default to be revisited by whoever next
looks at the binary size. Do not propose `-O1`/`-O2`/`-Os`/LTO as a size or
speed lever, and do not benchmark it as one.

The history is in `pc/CMakeLists.txt:21-29`: `-O2` → wild-pointer crash loop
from boot; `-O1` → SIGBUS on the intro train scene; no `-O` → stable. An
earlier draft of this file argued those data points were confounded and that
`-O2`'s 48 % `.text` cut (~3 MB) might make it "a budget requirement, not an
optimization." **That argument is retired.** A 3 MB saving on an image that
does not run is worth nothing, and the RAM plan (PLAN §3.1) is built entirely
from layout-class levers instead.

What is allowed, because it does not change instruction selection:
`-ffunction-sections -fdata-sections` + `-Wl,--gc-sections`, `.bss`
right-sizing, linker script placement, moving data to `/cd`, and dropping
non-goal subsystems. Codegen is banned; layout is fair game.

`DECOMP_OPT` remains settable only as a diagnostic escape hatch — e.g. to
confirm that a suspected miscompile is optimization-dependent. `-O0` here
literally means `-O0`, not "omit `-O`": `$KOS_CFLAGS` already carries `-O2`
and the last `-O` on the command line wins.

**Gate, if a per-TU exception is ever argued on measured evidence:** a full
new-game intro on hardware (KK Slider → train → town arrival). That is the
sequence that historically exposed the alignment bug class.

---

## How the flags are put together

`kos-cc` / `kos-c++` prepend `$KOS_CFLAGS` (`-ml -m4-single`, KOS include
paths, `-ffunction-sections -fdata-sections`, `-O2`, `-g`). The Makefile's own
flags come **after**, so they win on conflicts. Never call `sh-elf-gcc`
directly — the wrappers are what keep us ABI-compatible with the prebuilt
`libkallisti` / newlib / `libstdc++`.

Per-TU language handling mirrors `pc/CMakeLists.txt`:

```
decomp C (default)                        -w -std=gnu89 -fpermissive
jaudio_NES/*, libforest/emu64/*           -w -std=gnu11 -fpermissive
emu64.c, ja_calc.c, jammain_2.c, game64.c compiled as C++ (-x c++)
decomp C++                                -w -fpermissive -fno-exceptions -fno-rtti
dc/src/*.c                                -O2 -std=gnu11 -Wall -Wextra
src/main.c                                -Dmain=ac_entry     (KOS owns main)
src/static/boot.c                         -Dmain=boot_main
```

UB guards on all decomp code: `-fno-strict-aliasing -fwrapv
-fno-delete-null-pointer-checks -fno-lifetime-dse
-fno-aggressive-loop-optimizations -fno-strict-overflow -fno-stack-protector
-fsigned-char`. Each is justified per-flag in `kb/design-shelf-hazards.md`
§3.1 — with one correction: **`-fno-builtin` is deliberately not used.** §3.1
calls it "KOS convention (`KOS_CFLAGS`, VERIFIED)", but this image's
`$KOS_CFLAGS` does not contain it, and it breaks the link: it stops GCC
expanding `__builtin_alloca` inline, so `src/game/m_select.c:936,993` emit
calls to a real `alloca` symbol that newlib does not provide. There is no
`-fbuiltin-alloca` to re-enable it selectively.

`-DTARGET_PC` is **non-negotiable**. It means "not GameCube", not "PC": it
guards the base port's little-endian correctness fixes (byte-wise texconv in
`emu64.c`, the swapped `u16` pair ordering in `sys_matrix.c`, the overlap-safe
`Jac_bcopy` in `sample.c`). `-DTARGET_DC` is added alongside it for branches
that are genuinely Dreamcast-only.

---

## Include-path order is load-bearing

```
-Idc/include  -Ipc/include  -Iinclude  -Isrc  -I.
```

`dc/include` **must** come first so that:

* `dc/include/pc_platform.h` (an SDL-free shim) wins over the real
  `pc/include/pc_platform.h`, which pulls `<SDL.h>` and `<sys/mman.h>`. Five
  decomp TUs include it directly: `src/graph.c`, `src/game.c`,
  `src/game/m_play.c`, `src/game/m_actor.c`,
  `src/static/libforest/emu64/emu64.c`.
* `dc/include/SDL.h` (six symbols, not SDL) satisfies
  `src/static/jaudio_NES/internal/os.c`'s `#include <SDL.h>`.

`include/libc` is deliberately **not** on the path — having both `include/` and
`include/libc/` breaks the decomp's `#include_next` shadow-header chains, the
same as on PC.

---

## `dc/include/dc_prelude.h`

Force-included into every TU with `-include`. It exists because several
KOS/newlib identifiers collide with decomp identifiers in ways no include
ordering can fix (the decomp reaches `arch/arch.h` transitively through
`include/dolphin/types.h` → `<stdio.h>`). It handles exactly four things:

| Collision | Fix |
|---|---|
| `arch/arch.h:34` `#define page_count …` vs `u8 page_count;` in `m_notice_ovl.h`/`m_address_ovl.h` | pull `arch/arch.h` first, then `#undef page_count` |
| `<unistd.h>` `int link(const char*, const char*)` vs `typedef struct link_ link;` in `audiostruct.h` | rename **only** the POSIX declaration while pulling `<unistd.h>` in, then give the identifier back |
| KOS `dc/fmath.h:109` `static inline float fsqrt(float)` vs `math64.h:34` `#define fsqrt(x) sqrtf(x)` (which would rewrite KOS's *definition* into a static `sqrtf`) | include `dc/fmath.h` before any decomp header can define the macro |
| newlib's C++ headers don't pull `<string.h>` transitively (glibc's do) — breaks `JUTFont.h` / `JFWDisplay.cpp` | `#include <string.h>` |

Do not put game declarations in it.

---

## `pc/src` files compiled into the Dreamcast build

Three of them, listed in `PC_REUSE_C` in `dc/Makefile`. They are
platform-independent logic sitting *above* a backend that `dc/src` supplies,
classified as reusable by `kb/design-platform-api.md` §2a/§2b:

| file | why it is here |
|---|---|
| `pc_assets.c` | 30 677 LOC generated asset dispatch table. Its `pc_disc_*` backend is implemented in `dc/src/dc_dvd.c`. |
| `pc_save_bswap.c` | GCI byte-swap tables; §2a "truly verbatim". |
| `pc_m_card.c` | re-implements the game's own `src/game/m_card.c`, which **both** builds exclude. Without it the link is short 29 `mCD_*` / `pc_save_*` symbols. It compiles clean for sh-elf as-is, but **compiling is not working**: its `.gci`-file backend still has to become VMU/vmufs (PLAN §6 — ~100 KB of VMU for a ~456 KB GC save). |

`pc_settings.c` is *not* included: it uses `SDL_DisplayMode` at
`pc_settings.c:366-385`. `pc_disc.c` is not either — §2b moves it to `tools/`
as host code.

---

## Where this build currently stands

`make all` links and `mkdcdisc` produces a CDI. **It does not boot — it is too
big to load.**

```
   text      data       bss   image span   image end
6318552   2638852  12415508   21374068   0x8d472874
```

KOS's `_arch_mem_top` for a stock console is `0x8d000000`, so the image ends
past the top of RAM before any heap is touched.

`harness/dc/smoke.sh dc/build/OpenCrossing.cdi` returns `timeout` with **zero
bytes of console output** — no KOS banner at all. Attributed by experiment
(`kb/mem-budget.md` §8.7): a KOS hello-world containing nothing but a 21 MB
`.bss` array fails identically at essentially the same image end, while the
same hello-world with 4.7 MB (end under `_arch_mem_top`) passes in 3.08 s, and
the harness's own `selftest.cdi` passes in 3.10 s. **The guest never executes
an instruction, so there is no PC to symbolise**, and
`-DDC_NO_CRASH_PROTECTION` cannot distinguish anything here. Nothing about the
port's correctness is testable until the image fits.

Being under `_arch_mem_top` is **not** the bar. KOS's `mm_sbrk()` starts at the
ELF `end` symbol, with no MMU and no lazy commit, so every `.bss` byte destroys
a heap byte. The fit is **one inequality**, and stating it as two pools has
already produced two wrong numbers:

```
(image span) + (genuinely additive heap) ≤ 16,646,144
  21,374,068  +  3,545,184   ⇒ over by 8,273,108 B
```

All of it has to come out of **layout, not codegen** — see the optimization
section above.

**Live numbers: `kb/STATE.md`. The ranked levers: `kb/levers.md`. What is
already ruled out: `kb/closed.md`** — read that one before proposing anything
here. This section deliberately does not duplicate them; it went stale twice
when it did.

---

## Adding a source, or excluding one

Sources are discovered by `find` + a single ERE of exclusions, reproducing all
35 filters from `pc/CMakeLists.txt` verbatim. `dc/src/*.c` is globbed, so a new
platform file needs no Makefile edit.

`src/` is **vendored decomp**. When a TU genuinely cannot compile for SH-4, add
it to `DC_EXCLUDE_C` / `DC_EXCLUDE_CXX` in `dc/Makefile` with a reason and a
date — do not hack `src/` in place. Prefer a shim in `dc/include/` over an
exclusion; every one of the 12 known sh-elf failures
(`kb/design-shelf-hazards.md` §2) is handled by a shim, not an exclusion.

---

## Troubleshooting

**`docker image 'opencrossing-dc:sdk' not found`** — run
`bash dc/build-dc-image.sh`. Stage 1 (the toolchain) is ~24 min and is cached
forever; stage 2 is ~2.5 min.

**`No rule to make target 'build/obj/…'`** — the makefile's object paths are
absolute. Ask for `/work/dc/build/obj/src/foo.c.o`, not a relative path.

**Crash on hardware / in Flycast** — build keeps `-g` (free at runtime, and the
ELF never ships on the disc; only `1ST_READ.BIN` does). Feed the reported PC to
`sh-elf-addr2line -f -e dc/build/AnimalCrossing.elf <pc>`. For alignment faults
specifically see `kb/design-shelf-hazards.md` §4 — SH-4 traps every misaligned
16/32-bit access, a bug class no previous port of this codebase has ever
exercised.
