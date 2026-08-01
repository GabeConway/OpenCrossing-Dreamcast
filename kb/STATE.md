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
  `_arch_mem_top` is `0x8d000000`. It links, it will not boot. ~6.5 MB must
  come out, plus heap headroom.
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
3. **`.bss` right-sizing — 13.5 MB of the 22.5 MB ELF.** In flight, and note
   that lever 2 likely accounts for ~8.2 MB of it, leaving ~5.3 MB to
   classify. The
   framing that matters: this game shipped on **N64 in 4 MB of RDRAM**, and
   the GC build runs it through the in-house `emu64` layer. Buffers sized for a
   24 MB main + 16 MB ARAM GameCube, or for an emulated N64 memory image, are
   not sized for need. `emu64_utility.c:40` has a `seg2k0` heuristic keyed on
   N64 KSEG0 (`0x80000000`) — strong hint that an emulated memory image is in
   there. Classify every large symbol as *emu64/N64-emulation*,
   *GameCube-sized*, or *genuinely required on 16 MB*; the classification is
   worth more than the byte count.
4. **`--gc-sections`** with `-ffunction-sections -fdata-sections`. Pure
   layout, no codegen change.
5. **Drop non-goal subsystems.** NES emulation is a documented non-goal;
   `famicom.arc` is 1.70 MB on disc and the jaudio_NES tree pulls in the SDL
   shim.
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
2. emu64's GC address-range assumptions (`0x80000000`–`0x83000000`) vs KOS's
   `0x8c000000` RAM base — the `seg2k0` heuristic must be re-derived.
3. Does `emu64` emulate an N64 CPU/RSP, or only translate microcode to GX?
   This decides whether a whole RDRAM-sized buffer exists in our `.bss`.

## Disc-space facts (settled — stop re-measuring)

Image 1,459,978,240 B; **real content 27,573,513 B (1.89%)**, the rest
trailing zeros, confirmed byte-by-byte rather than sampled. Recommended DC
layout with `foresta.rel` Yaz0-expanded = **35.24 MB, 5.3% of a CD-R.** Disc
capacity is a non-issue — spend it freely on offline conversions that trade
disc space for SH-4 cycles. Biggest files: `audiorom.img` 8.30 MB,
`foresta.rel.szs` 6.14 MB → 15.64 MB expanded, `foresta.map` 4.85 MB,
`forest_2nd.arc` 4.13 MB, `famicom.arc` 1.70 MB (droppable).

## Next actions

1. Land the `.bss` classification and apply the layout levers; be honest if
   they do not close 6.5 MB — "still N MB short with `-O0` mandatory" is a
   valid and important result, not a failure to report.
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
