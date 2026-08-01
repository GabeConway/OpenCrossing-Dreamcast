# Session state — resume here

Last updated 2026-08-01, second execution session (in progress). Written so a
fresh context can pick up without replaying anything.

## Headline

**M0 and M1 gates are met. The blocker is RAM, and it is a hard one.**

- **3917 / 3917 translation units compile for sh-elf**, zero exclusions, and
  `src/` was not modified — every compat fix lives in `dc/include/dc_prelude.h`
  as a force-include. It also links and produces a 27 MB unpadded CDI.
- **The linked ELF is 22.5 MB against a 16 MB machine.** text 6,318,568 +
  data 2,638,852 + bss 13,526,548. It ends at `0x8d581c14`; KOS's
  `_arch_mem_top` is `0x8d000000`. It links, it will not boot.
- **The cut required is ~14.45 MB, not the ~6.5 MB an earlier pass reported.**
  6.5 MB only gets the image under `_arch_mem_top` with ~1.2 MB of heap left,
  which is less than the game's first archive mount. Against the ledger's own
  7.61 MB heap (`dc/include/dc_mem_budget.h` buckets 6–12) plus KOS's ~1 MB,
  **the image budget is 8,035,072 B** and the image is 22,486,548 B. Measure
  every proposal against 8,035,072 B, not against 16 MB.
- **Why `.bss` is not free:** KOS's `mm_sbrk()` starts at the ELF `end`
  symbol. No MMU, no lazy commit. **Every `.bss` byte literally destroys a
  heap byte.** This is the fact the whole size problem turns on.
- **Compiler optimization is not available to close that gap** — see the
  standing constraint below. `.bss` right-sizing, `--gc-sections`, dropping
  non-goal subsystems, and moving data to `/cd` are the levers.

## Standing constraint added this session (user directive)

> "the optimizations cause problems and we cant use them without the port
> being broken"

**`-O1` / `-O2` / `-Os` / LTO are banned as a strategy.** Not "risky" —
banned. The armhf record is why: `-O2` gave a wild-pointer crash loop from
boot, `-O1` gave a hard SIGBUS on the intro train scene. Game code (`src/`)
builds at **`-O0`, and that is a fixed input to every RAM plan.**

The useful distinction is **codegen vs layout**:

| Lever | Changes instruction selection? | Allowed |
|---|---|---|
| `-O1/-O2/-Os`, LTO | yes | **no** |
| `-ffunction-sections -fdata-sections` + `-Wl,--gc-sections` | no | yes |
| Right-sizing `.bss` arrays / arenas | no | yes |
| Dropping non-goal subsystems (NES, `famicom.arc`) | no | yes |
| Moving rodata/tables to `/cd`, loaded on demand | no | yes |
| Linker script placement | no | yes |

Anything in the "yes" rows is fair game and does not need re-litigating.

## What exists and is verified

**Toolchain (M0).** `opencrossing-dc:sdk` in the local Docker daemon:
sh-elf GCC 15.2.0, newlib 4.6.0.20260123, binutils 2.45.1, KOS 2.3.0
(`1c6398f9`), kos-ports (`f4faacc4`), GLdc (`a1cd80a8`), mkdcdisc
(`3c2ef63a`), `-m4-single`, thread model kos, **char is SIGNED by default on
this build** (so `-fsigned-char` is belt-and-braces, not load-bearing).
Cold rebuild ≈ 27 min (24 stage 1 + 2.5 stage 2). Host has **no BuildKit** —
`DOCKER_BUILDKIT=0`, never pass `--progress`.

- `dc/Dockerfile`, `dc/build-dc-image.sh` — build the image (idempotent).
- `dc/build-dc.sh` — **host-side** build wrapper (this is the one you run).
- `dc/build-dc-docker.sh` — runs **inside** the container; not a host entry
  point. (`CLAUDE.md` originally advertised it as one; it never was.)
- `dc/Makefile` — plain GNU make. Objects in `dc/build/obj` mirroring source
  paths. `DC_EXCLUDE` list exists and is **empty**. Uses make's `$(file …)`
  for a linker response file: 3900 object paths exceed `execve`'s `ARG_MAX`.

**Harness (M0).** Verified against real CDIs, not asserted:
`smoke.sh` exits 0 on selftest and 1 on crashtest; `crash.sh` resolves the
fault to `crashtest.c:39`; `perf.sh` passes in-band and fails on a shifted
baseline. Band = `max(2% of baseline, 100 µs)`. Symbolisation runs
`sh-elf-addr2line`/`objdump` in the SDK container against `<image>.src.json`
provenance sidecars (abs path + sha256 + size + built_utc + image + producer).

**Platform layer.** `dc/src/dc_{os,gx,vi,pad,audio,aram,dvd,card,misc,stubs,
mem_ledger,main,mtx}.c`. `dc_main.c` boot order: `dc_mem_ledger_init()` →
`pc_settings_load/apply()` → `dc_platform_init()` (`vid_set_mode(DM_640x480,
PM_RGB565)`) → `/cd` check → `pc_assets_init()` → `ac_entry()` →
`boot_main()`. PVR init deliberately stays in `dc_gx.c`.

`dc_mtx.c` implements all 37 `PSMTX*`/`PSVEC*`/`C_MTX*`/`gu*` symbols.
XMTRX behind `DC_MTX_USE_XMTRX` (default 1); FIPR/FSRRA behind
`DC_MTX_USE_FIPR` (**default 0** — both are ~20-bit approximations).
`PSMTXConcat(a,b,r)` loads **b untransposed** and FTRVs each row of a (the GX
column-vector and SH-4 row-vector conventions cancel); `PSMTXMultVec/SR/Array`
load an explicit `transpose(m44)`. Verified numerically against the scalar
reference with a host-side FTRV simulator. `PSMTXInverse` left scalar.

**Asset tooling.** `tools/dcasset/` extracts the real ISO and has a `relmap`
subcommand; `pack.py` / `assets_scan.py` are landing now. See
`kb/asset-pack.md`.

## Known-unreviewed and known-wrong

Every kb deliverable from the first session was written by agents whose
**adversarial verifiers all died**. Treat kb numbers as claims. Two have
already been falsified by contact with the real toolchain:

1. `kb/design-shelf-hazards.md` §3.1 marked `-fno-builtin` "(VERIFIED)" as KOS
   convention. **False for this image.** `$KOS_CFLAGS` does not contain it,
   and adding it makes `m_select.c:936,993` emit calls to a real `alloca` that
   newlib does not provide. There is no `-fbuiltin-alloca`. Dropped.
2. §2.3's header-collision scan predates this image (it assumed GCC 9.3 / KOS
   `525cbda`), so it missed both collisions we actually hit — see below.

**Unverifiable offline:** `dc_main.c` rewrites `CONTEXT_PC(*ctx)` to a
trampoline so exception recovery runs in thread context. If that is wrong the
failure mode is a silent boot hang. Triage flag: `-DDC_NO_CRASH_PROTECTION`.

## Traps already paid for — do not re-discover these

- **`fsqrt`**: KOS `dc/fmath.h:109` defines `static inline float fsqrt(float)`;
  the decomp's `math64.h:34` `#define fsqrt(x) sqrtf(x)` rewrites KOS's
  *definition* into a static `sqrtf` that collides with newlib.
- **POSIX `link()` vs the decomp's `typedef struct link_ link`**, arriving via
  `<stdio.h>` → `<sys/stdio.h>` → `<unistd.h>`. A blanket `-Dlink=` does not
  work — it renames both sides. The prelude renames only the POSIX
  declaration, then restores the identifier.
- **Guest `scif_flush()` permanently kills the Flycast console.** KOS's flush
  clears TEND and spins; Flycast never re-raises TEND on an idle TX FIFO; KOS
  latches `serial_enabled = 0`; a later crash then prints **nothing**.
  Bisected across 7 guest variants — raising baud is fine, the flush is the
  killer. Removed from `selftest.c`/`crashtest.c`. Never call it.
- **KOS 2.3 assertion text** is `*** ASSERTION FAILURE ***` /
  capital-A `Assertion "x" failed` — the documented lowercase regex never
  matched, so a failed `assert()` only ever showed up as a timeout.
- **`bash -lc` in the SDK image** re-runs `/etc/profile` and drops
  `/opt/toolchains/dc/sh-elf/bin`, so `sh-elf-addr2line` vanishes and every
  address silently symbolises to `??`. Use `bash -c`.
- **Sourcing `environ.sh` under `set -u`** exits 127 with nothing on stderr.
- **mkdcdisc padding**: default = 740,083,145 B / 15.6 s; `-N` = 1,783,337 B /
  0.021 s. 415× the size and 740× the time. Use `-N` for every emulator run.

## The RAM levers, ranked

1. **Resident REL blob — 16.56 MB. SOLVED, tool built and verified.**
   `pc_assets.c` kept the whole decompressed `foresta.rel` (15.64 MB) plus
   `main.dol` (0.92 MB) resident, i.e. more than the entire machine.
   `dcasset pack` emits **`assets.pak`, 8,917,568 B**, covering 16,365
   references over 8,787,262 distinct blob bytes as 3,188 chunks, with a
   **51,104 B resident index** (0.3% of RAM). Net saving **15.68 MB**. Round
   trip: 16,365 references replayed over 8,884,894 B, **zero mismatches**.
   Chunks are laid out in real load order and **pre-byte-swapped offline**, so
   the SH-4 never runs `do_swap`; 82 backward reads with max reach 7,520 B mean
   an **8 KB window gives zero seeks** — the whole load is one linear 8.9 MB
   read (17.8 s at 500 KB/s). The pack also replaces `foresta.rel` on disc
   (−6.7 MB, no Yaz0 at boot). The old 8.66 MB lower-bound caveat is **closed**:
   reconciliation is exact at 2,641 sites with zero unaccounted, and `pack`
   refuses to build if that ever goes non-zero. Details: `kb/asset-pack.md`.
   Remaining work is the runtime loader in `pc_assets.c`.
2. **Asset destination arrays — 8.22 MB. The new floor, and probably most of
   `.bss`.** The assets land in **15,726 static arrays totalling ~8,617,214 B**
   that stay resident no matter where the bytes came from. Lever 1 removes a
   *peak*; this is a *floor*, and it is now the largest single line in the
   16 MB budget. The pack format already supports the fix — every asset is
   individually addressable, so demand-loading into pooled storage is a
   **loader-only change, no codegen**.
3. **`.bss` right-sizing — 13.5 MB of the 22.5 MB ELF.** Measured with `nm -S`
   over 3,621 objects: **`src/data/**` = 8,519,191 B = 64% of all BSS**, which
   matches `kb/mem-budget.md` to the byte *and* corroborates lever 2's
   8,617,214 B of destination arrays from a completely different direction.
   By directory: `src/data/model` 5,682,621 · `src/data/npc` 1,593,792 ·
   `src/game` 1,548,236 · `jaudio_NES` 1,265,101 · `src/data/field` 1,144,896 ·
   emu64 562,374. Top singles: `prbuf` 1,228,800 (`m_play.c:54`) ·
   `audiomemory` 589,824 (`game64.c_inc:587`) · `texture_buffer_data` 524,288
   (`emu64.c:41`) · `pc_m_card.c` save staging 320,150 · `aSTR_overlay`
   294,912 · `sys_dynamic` 132,104.

   Two one-line wins, high confidence, no new machinery:
   - **`prbuf`: `sizeof(u32)` → `sizeof(u16)`, −614,400 B.** Only writer is
     `copy_efb_to_texture()` doing `GXSetTexCopyDst(640, 480, GX_TF_RGB565, 0)`
     = 2 B/px; only reader binds `G_IM_SIZ_16b`. The 2× is dead in retail too.
   - **Revert the PC texture-cache inflation, −496,640 B, three lines.**
     `texture_cache.h` reads `0x80000 /* 512 KB (was 48 KB) */`; retail GC
     values sit right below at lines 74/75/16 (`0xC000`, `0x400`, `256`).
     Sufficiency is proven by the shipped product.

   **`DC_MAIN_MEMORY_SIZE` (4,000,000) is heap-allocated at `dc_os.c:400`** —
   4 MB *on top of* the 22.5 MB image, not inside it. **The true overage is
   counted against the heap side of the budget, not the 22.5 MB image.**

   ⚠️ **The emu64/N64-emulation lead is a red herring — do not chase it
   again.** `emu64` is a GBI display-list interpreter that emits GX, **not** a
   machine emulator, and there is no emulated RDRAM in the tree.
   `emu64.hpp:750`'s `u32 segments[16]` is 64 bytes of real GameCube pointers;
   `seg2k0()` bounds-checks `0x80000000..0x83000000` because that is GameCube
   MEM1, not because it is an emulated image's extent. The game logic is
   *ported*, not emulated — `src/` carries the same TUs as the N64 decomp and
   `src/static/libultra/` reimplements the N64 OS API on Dolphin OS. The only
   genuinely emulated memory in the build is rspsim's 4 KB `DMEM[0x1000]`.
   Whole emu64 layer = 648,229 B with ~13.5 KB of genuine state; the NES
   emulator is 83 KB. Neither is a lever. See `kb/research-n64-origin.md`.

   **What the N64 original *does* give us is its memory model:** 4 MB RDRAM,
   no Expansion Pak, assets and code overlays DMA'd out of the 16 MB cart on
   demand (`src/dmadata`, `src/boot/{m_std_dma.c,ovlmgr.c,yaz0.c}`), heap
   running from end-of-BSS to the framebuffer — "reserve as little statically
   as possible". That is the opposite of what this build does, and it is the
   shape to aim for, with the CD-R as the cart. The N64 decomp is
   **zeldaret/af** (active; supersedes BluRosie/doubutsu-no-mori); no PC port
   exists and both `af` and `ac-decomp` declare porting a non-goal.
4. **Everything else, −4.32 MB combined** (from `kb/research-size-reduction.md`,
   measured against the real ELF + map): `.data` `src/data` tables to disc
   −1.94 MB (0.95 MB pointer-free today, 0.99 MB needs a REL-style reloc pass)
   · `prbuf` → PVR render target −1.23 MB (supersedes the u16 halving above;
   take the halving as the cheap step, the render target later) · `s_assets[]`
   name-string pool → disc index −0.89 MB (888,853 B in `pc_assets.c.o` alone)
   · `audiomemory`/jaudio → AICA + shrink −0.65 MB · emu64
   `texture_buffer_data` → decode straight to VRAM −0.52 MB · actor overlay
   staging arenas → one shared union arena −0.46 MB · `pc_m_card` −0.28 ·
   `dc_gx` −0.24 · delete NES-emu/texpack/viewer −0.11.

   **Grand total −14.77 MB against −14.45 MB required: it closes on paper with
   a 2% margin.** "Closes on paper" is not "safe".

## Dead ends — verified, do not re-propose

- **`--gc-sections` is already applied and already spent.** The map's discard
  block is 29,471 sections recovering **522,150 B of allocatable RAM, and that
  is all there is.** GCC does not emit unreferenced `static`s even at `-O0`,
  which is why it is ~292 KB of `.text` and not 2 MB.
- **`--icf` is unavailable on SH** — gold's `configure.tgt` has no SH backend,
  `ld.bfd` has no ICF, `lld` has no SH port.
- **SH GCC has no small-data model** — no `-G`/`-msdata` in
  `gcc/config/sh/sh.opt`. The `.sdata`/`.sbss` in the KOS linker script are
  inert boilerplate, 0 bytes in the map.
- **`-mrelax` / `-Wl,--relax` is a codegen change** (emits `.uses` pseudo-ops
  so `ld` can synthesise `bsr`). Disqualified under the `-O0` directive.
- **Stripping debug info saves 0 RAM** — 67 MB of the 72 MB ELF is
  non-`SHF_ALLOC`; `objcopy -O binary` never emitted it.
- **Compressing `1ST_READ.BIN` saves 0 RAM.** `.bss` is `NOBITS`; compression
  is arithmetically incapable of touching the actual problem.
- **AICA's 2 MB is not usable as a size lever.** Genuine ARAM analogue but
  DMA-only: G2 is 16-bit @ 25 MHz, ~40 MB/s, with a FIFO protocol KOS itself
  calls "a pain in the rear." You cannot put a C array there. Budget 0 MB.

## On the shelf

- **Code overlays are real and shipping on DC** — ScummVM's
  `backends/platform/dc/dcloader.cpp` + `plugin.x`, an SH-4 ELF loader
  handling exactly `R_SH_DIR32`, in production since 0.7.0. Shelved because it
  buys `.text`, and `.text` is only 5.26 MB of a 22.5 MB problem.
- **VRAM as a store is legitimate** — KOS's own linker script demonstrates the
  mechanism (`.ocram 0x7c001000 (NOLOAD)`). But ~half of VRAM is off-limits
  during PVR rendering, reads are slow, and there is **no citable measured
  SH-4↔VRAM read figure** — UNVERIFIED, must be measured before use.
- **Drop non-goal subsystems.** NES emulation is a documented non-goal;
  `famicom.arc` is 1.70 MB on disc and the jaudio_NES tree pulls in the SDL
  shim. Small (−0.11 MB of RAM) but free.
6. **`foresta.map` / `static.map` — 5,402,023 B of disc. SETTLED: droppable.**
   The only reader is `JUTException::queryMapAddress_single()`
   (`JUTDirectFile::fopen` → `DVDOpen`), reachable only via
   `showMapInfo_subroutine` ← `showStack`/`showGPRMap` ← `printDebugInfo` ←
   `errorHandler`, installed by `OSSetErrorHandler`. Two independent kills on
   DC: the REL is never loaded as a module, and `showMapInfo_subroutine`
   returns false for any address outside `0x80000000..0x82FFFFFF` — **no SH-4
   address qualifies**. `boot.c:695`'s `setMapFile("/static.map")` only pushes
   a string; `dvderr.c` has no map reference at all.

## Open questions

1. Real JKRHeap high-water marks — instrument the PC build, cheap there.
2. `emu64`'s `seg2k0` bound is `0x80000000..0x83000000` (GameCube MEM1) and
   **still has to be re-derived for KOS's `0x8c000000` RAM base** — every
   resolved segment pointer on DC will fail that check. Answered: it is a
   validity assert on real pointers, not an emulated-memory extent (q3 below
   is closed), so the fix is re-pointing the bound, not removing a buffer.
3. ~~Does `emu64` emulate an N64 CPU/RSP?~~ **Closed: no.** GBI interpreter
   emitting GX. See lever 3.
4. `dc/Makefile:165` defines `-DTARGET_PC`, so the DC build takes
   `pc_assets.c`'s eager-load branch (8,771,358 B at boot into `#ifdef
   TARGET_PC` placeholders). Confirm that is deliberate and audit what else
   that define drags in.

## Disc-space facts (settled — stop re-measuring)

Image 1,459,978,240 B; **real content 27,573,513 B (1.89%)**, the rest
trailing zeros, confirmed byte-by-byte rather than sampled. Recommended DC
layout with `foresta.rel` Yaz0-expanded = **35.24 MB, 5.3% of a CD-R.** Disc
capacity is a non-issue — spend it freely on offline conversions that trade
disc space for SH-4 cycles. Biggest files: `audiorom.img` 8.30 MB,
`foresta.rel.szs` 6.14 MB → 15.64 MB expanded, `foresta.map` 4.85 MB,
`forest_2nd.arc` 4.13 MB, `famicom.arc` 1.70 MB (droppable).

## Next actions

0. **Measure the three unknowns first — one day's work, and the plan's 2%
   margin depends on them.** KOS+GLdc baseline RAM; `pvr_mem_available()`
   after a real texture load; `__osMalloc` peak. Ledger bucket 6 (4.0 MB
   `JKRHeap`/`__osMalloc`) is **entirely unmeasured** — if it is really 6 MB
   the whole plan is 2 MB short and we should know that before building on it.
1. **The `src/data` demand-residency conversion is the milestone.** −8.45 MB,
   57% of the required cut, and it also deletes the 15.64 MB
   `foresta.rel.szs` boot transient, which is independently a hard blocker on
   a 16 MB machine. Nothing else is within a factor of five, and with `-O0`
   frozen there is no second lever of that magnitude. Needs the asset pack's
   runtime loader (next item) first.
   Be honest if the levers do not close the gap — "still N MB short with
   `-O0` mandatory" is a valid and important result, not a failure to report.
2. Wire the runtime loader in `pc_assets.c` to `assets.pak` (lever 1). Two
   loader rules from the pack author: **log window faults, never swallow
   them** (a regenerated `pc_assets.c` that reorders calls silently degrades
   to `fs_seek` + binary search — correct but minutes slower), and **do not
   delete `do_swap`** (a future regeneration with a swap conflict ships that
   chunk raw with the `PRESWAPPED` bit clear).
3. Demand-load the 8.22 MB of destination arrays into pooled storage
   (lever 2) — the biggest remaining line, and loader-only.
4. Boot the real game CDI under `harness/dc/smoke.sh`; report the symbolised
   crash point via `crash.sh`.

## Standing constraints

Stock 16 MB DC — the 32 MB mod must never become a requirement. No shaders,
no T&L, one texture unit. VMU ≈ 100 KB vs a ~456 KB GC save. CD-R ~500 KB/s,
so all disc I/O needs read-ahead. Game code stays `-O0` (above). Never commit
ROM material or built disc images. The user's ISO is at
`/Users/gabe/Documents/GitHub/OpenCrossing-Anbernic/harness/rom/Animal
Crossing.iso` — reference it, never copy it in. `pc/` is reference material,
not a build target. Agents must not run git; the main thread commits. Every
optimization gets a kill switch. Emulator-first iteration (Flycast), hardware
for truth; dev console is a known-good MIL-CD unit that boots burned CD-Rs.
