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
| `DC_ASSET_STUB` | `0` | `1` → the throwaway bring-up image (see below) |
| `DC_DISC_ROOT` | unset | a directory whose files go on the disc **flat** |
| `DECOMP_OPT` | `-O0` | optimization level for decomp game code |
| `DC_OPT` | `-O2` | optimization level for `dc/src` platform code |
| `DC_ARENA_BYTES` | header | arena size (bucket 6). **Shrink, never grow** — it competes with libc |
| `DC_ARAM_WINDOW` | header | resident graph-ARAM window. Floor 851,968 (`forest_1st.arc`) |
| `DC_DIAG` | `0` | `1` → `PC_DIAG()` bring-up tracing inside `graph_proc` |
| `DC_FB_PROBE` | unset | `<N>` → guest-side screenshot every N presented frames. Needs `smoke.sh --fb-writeback` to see anything |
| `DC_ARENA_PROBE` | unset | `<N>` → arena touched/used + libc break every N presented frames |
| `DC_ASSET_CENSUS` | unset | `1` → record the asset addresses the GX layer is handed; resolve with `tools/dcstub/census_resolve.py` |
| `DC_STUB_KEEP` | logo list | `:`-separated sources the stubber leaves FULL SIZE. Generate it from a census with `tools/dcstub/census_keeplist.py` |
| `DC_AUTOSTART` | unset | `<N>` → synthesise START/A from `PADRead` call N onward. **The only way an unattended run gets past the title screen** |
| `DC_AUTOSTART_PERIOD` | `90` | calls between synthesised pulses (each pulse is 6 calls) |
| `DC_AUTOSTART_START_EVERY` | `4` | every Nth pulse is START, the rest are A. **A-dominant on purpose** — past the title, dialogue takes A or B only (`m_msg_normal.c_inc:2`) and choice menus default to index 0, so a 1:1 alternation wasted half of every run. START is needed at exactly one place on the path to the town: ending the name-entry keyboard (`m_editor_ovl.c:447`). `=2` restores the old 1:1 pattern |
| `DC_CONSOLE_LIMIT` | `1` | `0` → kill switch for the `printf`/`OSReport` flood limiter |
| `DC_FB_IMAGE` | unset | `<1\|2\|4>` → with `DC_FB_PROBE`, stream the whole frame out as base64 `FBROW` lines, box-filtered by that factor. Decode with `tools/dcfb/fbimg_to_png.py` |
| `DC_TEX_LOG` | unset | `1` → one line per texture **upload** describing what the decoder produced. Separates "never uploaded" from "uploaded as a blank rectangle", which the uploads/hits/evictions counters cannot |
| `DC_PVR_BATCH_LOG` | unset | `<N>` → dump every renderer batch's state on every Nth frame. Set it to the **same** N as `DC_FB_PROBE` so the lines describe the captured frame. Fields: `ac=` alpha compare asked for, `cut=` treated as a cutout, `cu=` colour/alpha update, `tm=` stage 0/1 texmap (`255` = `GX_TEXMAP_NULL`), `st=` TEV stage count, `t1=` whether texmap1 binds a *different* image, plus screen bbox, z and uv ranges |
| `DC_ARAM_TBL_PROBE` | unset (0) | `1` → log every 64-byte ARAM read. That size is uniquely `mMsg_Get_BodyParam`'s resource-TABLE fetch (`m_msg_main.c_inc:284,289`), and MESSAGE (works) and STRING/SELECT (broken) both use it, so one run gives the control and the failure side by side. Decision table is in the comment at the probe |
| `DC_ARAM_AUDIO_DROP` | `1` | `0` → stop discarding jaudio's ARAM half, so `audiorom.img` becomes disc-backed and synthesis has real samples. **Unproven and risky** — jaudio streams ~8.5 MB *before* `JW_Init2` mounts `forest_1st.arc`, so it can exhaust the extent table and make archive content anonymous. A/B the `[DC/ARAM] LRU` line: `LOST=0`, `ext` under the cap, `mapped=` unchanged |
| `DC_AUDIO` | `1` | `0` → no output device, no synthesis pump. Audio is ON by default but currently produces silence (see `kb/RESUME.md` item 8) |
| `DC_AUDIO_BUDGET_US` | `4000` | per-frame ceiling on jaudio synthesis; on exhaustion the pump drops an audio frame rather than stalling the game loop |
| `DC_AUDIO_HEADROOM` | `2048` | samples kept free at the top of the ring. ⚠️ Do **not** gate the pump on "ring less than half full" — a stalled consumer then leaves it half full forever and synthesis never runs (measured deadlock, `synth_frames=0`) |
| `DC_SCIF_FAST` | unset | `1` → raise the console from KOS's 57,600 baud to **1,562,500** (~5.8 KB/s → ~150 KB/s), the rate the harness selftest has used since M0. ⚠️ **Emulator only** — a real coder's cable will not sync at 1.5 Mbps and a hardware build with this set has no console and no crash dump. What it buys: a `DC_FB_IMAGE` screenshot drops from ~35 s to ~1.4 s, so a screenshot run stops being a different experiment from a progression run |
| `DC_XDEFS` | unset | raw extra `-D` flags, appended last. How the renderer kill switches are reached — see below |
| `V` | unset | `V=1` echoes full compiler command lines |

### Renderer kill switches (`DC_XDEFS`)

Compile-time A/B knobs in `dc/src/dc_pvr.c`. Each isolates one convention that
has already been got wrong at least once, so a single build settles the
question instead of an argument from source.

| define | effect |
|---|---|
| `DC_PVR_NO_UVCLAMP` | ignore the GX wrap mode; every texture repeats (the pre-2026-08-02 behaviour) |
| `DC_PVR_NO_TEVCONST` | ignore TEV constant colours; stage 0's colour is always the rasterised colour |
| `DC_PVR_NO_CULL` | `PVR_CULLING_NONE` everywhere |
| `DC_PVR_CULL_INVERT` | swap CW/CCW — restores the old, wrong cull mapping |
| `DC_PVR_NO_LIGHTING` | skip GX channel evaluation, pass the vertex colour through |
| `DC_PVR_NO_NEARCLIP` | drop straddling triangles instead of clipping them |
| `DC_PVR_NO_TEXTURES` | untextured backend |
| `DC_PVR_NO_TEXNULL` | restore the old behaviour where a draw with `GX_TEXMAP_NULL` still inherited `tex_handle[0]` — i.e. 2D panes sampling a stale texture's texel (0,0) |
| `DC_PVR_NO_ALPHATEST` | restore the old cutout handling: alpha-tested batches keep `src=ONE dst=ZERO`, so fully transparent texels paint at full opacity and write depth |
| `DC_PVR_NO_COLORMASK` | ignore `GXSetColorUpdate(GX_FALSE)`; depth-only passes paint solid geometry again |
| `DC_PVR_ALPHAENV` | ⚠️ **OPT-IN — measured to regress, off by default.** Gives a batch whose GX stage-0 alpha combiner is exactly `(ZERO, ZERO, ZERO, TEXA)` — 78 % of display-list sites, 67 % of runtime batches — `PVR_TXRENV_MODULATE` instead of `MODULATEALPHA`, so its alpha is the texel's alone rather than `vertex.a × texel.a`. That is the GX-correct answer (the vertex alpha byte there is the `G_RM_FOG_SHADE_A` **fog coefficient**, and this port fogs in PVR hardware), and it *does* clean up the dialogue balloon — but a 320×240 A/B over two 600 s runs shows the **train station canopy collapse to a flat teal slab**. Counters pass; the screenshots do not. Diagnosis and next experiment are in the comment at `alpha_env_texel_only()`. `[DC/PVR] alphaenv texel_only=` reports how often it fires |
| `DC_PVR_NO_PUNCHTHRU` | **the punch-through kill switch.** Disables `PVR_LIST_PT_POLY` (`opb_sizes[4]` back to `PVR_BINSIZE_0`), so no batch is deferred and every cutout goes back through the 2026-08-02 blend approximation in the single general list. Restores the pre-punch-through behaviour verbatim |

#### Punch-through tuning (all imply punch-through is ON)

The PVR has no alpha test outside `PVR_LIST_PT_POLY`, and that list is number 4
— last — so cutout geometry is transformed at submission time into a deferred
32-byte-record buffer and replayed after the general list closes. See the
"decision 1" block at the top of `dc/src/dc_pvr.c`.

| define | default | effect |
|---|---|---|
| `DC_PVR_PT_ALPHA_REF` | `144` | the global `PT_ALPHA_REF` register (`0xA05F811C`), pinned to emu64's `tex_edge_alpha` default. It is ONE value for the whole render — the PVR has no per-polygon reference |
| `DC_PVR_PT_BUF_RECS` | `2048` | deferred records, 32 B each = **65,536 B of `.bss`**. Raise it if `[DC/PVR] … ptdrop=` is ever nonzero |
| `DC_PVR_PT_BINSIZE` | `32` | PT object-pointer bin size. VRAM only, ~153,600 B per buffer set. `16` halves it |
| `DC_PVR_PT_ALL` | unset | also route **blended** cutouts (`GX_BM_BLEND` + alpha test) to PT. By default only `GX_BM_NONE` cutouts — opaque-with-holes — go there, because PT forces a passing fragment's alpha to 1.0 and would make a fading sprite pop opaque |

The `[DC/PVR] pt …` line printed next to `[PERF]` every 30 frames carries
`batches` / `verts` / `recs` routed to PT, `pthi=<worst frame>/<cap>` and
`ptdrop=<triangles refused by a full buffer>`. **`ptdrop` must be 0**; anything
else is cutout geometry missing from the screen. `DC_PVR_BATCH_LOG` gained a
`pt=` field next to `cut=`: `cut=1 pt=0` is a cutout the router declined.

⚠️ **A kill switch reverts one fix, not one symptom.** `DC_PVR_NO_UVCLAMP`
built to test the train windows also un-fixes K.K. Slider's spotlight, because
that is what the wrap fix repaired. Say what a given A/B is expected to break
before anyone looks at it.

```bash
DC_XDEFS='-DDC_PVR_NO_UVCLAMP' bash dc/build-dc.sh
```

```bash
DC_TARGET=objs bash dc/build-dc.sh     # compile-only
DC_CDI_PAD=1   bash dc/build-dc.sh     # CD-R burn image
JOBS=8         bash dc/build-dc.sh
DECOMP_OPT=-O2 bash dc/build-dc.sh     # see the warning below
DC_ASSET_STUB=1 bash dc/build-dc.sh    # bring-up image that actually boots
```

The image that currently gets furthest — past the title, into the train intro,
with real textures — is a stub image with a censused keep list and autostart:

```bash
python3 tools/dcstub/census_resolve.py <run>/console.log \
    --sizes-from dc/build/nonstub/AnimalCrossing.elf --top 0 > /tmp/census.txt
DC_STUB_KEEP="$(python3 tools/dcstub/census_keeplist.py /tmp/census.txt \
                 --with-default --colon)" \
DC_DISC_ROOT=~/.cache/oc-dc-discroot DC_ASSET_STUB=1 \
DC_ARAM_WINDOW=131072 DC_ARENA_BYTES=1900000 DC_AUTOSTART=300 \
  bash dc/build-dc.sh
```

### `DC_ASSET_STUB=1` — the bring-up image

The real image is 8,273,108 B over 16 MB, so it never executes an instruction:
startup `.bss` zeroing runs off physical memory before `scif_init()` and there
is not even console output. `DC_ASSET_STUB=1` runs
`tools/dcstub/make_stub_data.py` on the host first, which rewrites every asset
destination array to `[1]` (2,535 TUs, 16,317 arrays, 8,716,158 B) into
`dc/build/stubsrc`; `dc/Makefile` then compiles those TUs instead of their
`src/` originals. `src/` is untouched — the arrays are generator output, so
generating them small is a generator change, legal under the `-O0` rule
(CLAUDE.md §1).

The build also defines `-DDC_ASSET_STUB`, which makes `dc_main.c` skip
`pc_assets_init()` (the central table would memcpy full-size assets over
one-element destinations) and defaults `g_pc_verbose` to 1 (every `OSReport` in
the game is gated on it, and a burned CD-R passes no argv, so `--verbose` is
unreachable there). `-DDC_VERBOSE` turns the latter on by itself for a normal
build.

The game renders garbage the moment it reads an asset. That is the point: the
image exists to exercise the platform layer, not to look like Animal Crossing.
`bash dc/build-dc.sh clean` removes the stub tree with the rest of `dc/build`.

### `DC_DISC_ROOT` — putting real game data on the disc

Without it the CDI is built from the ELF alone, `/cd` mounts empty and every
`DVDFastOpen` misses — which is where the S1 image stops, in
`JKRAramArchive::open()` on a zero-byte `forest_1st.arc`.

`dc_dvd.c` builds every path as `"/cd" + "/" + name` (`dc/src/dc_dvd.c:113`),
so the files must sit at the **disc root, flat**. `dcasset extract` writes the
GameCube shape instead (`files/`, `sys/`), so it needs flattening first:

```bash
python3 tools/dcasset/dcasset.py extract "<the ISO>" --out /tmp/discroot
bash dc/stage-disc.sh /tmp/discroot /tmp/discflat        # 11 files, 36,953,162 B
DC_DISC_ROOT=/tmp/discflat DC_ASSET_STUB=1 bash dc/build-dc.sh
```

The directory is bind-mounted read-only at `/discroot` and passed to
`mkdcdisc -D`. **Keep the staging directory out of the repo** — it is ROM
material, and neither it nor the CDI may ever be committed (CLAUDE.md §1).

⚠️ `pc_disc_extract_rel()` (`dc/src/dc_dvd.c:290`) reads the whole 15,640,056 B
`foresta.rel` into RAM. On a 16 MB machine that cannot work; it is exactly what
`kb/levers.md` L2 replaces with `assets.pak`. It is only reachable from
`pc_assets_init()`, which the stub build skips — so the stub image is safe, and
a non-stub image with disc content is not.

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
