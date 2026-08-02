# Session state — resume here

Last updated 2026-08-01, end of the second execution session. Written so a
fresh context can pick up without replaying anything. Read this first, then
`CLAUDE.md`, then `PLAN.md`.

## Headline

**M0 and M1 are met. M2 is blocked on RAM, and the arithmetic says the port is
not yet known to be viable.**

- **3917 / 3917 translation units compile and link for sh-elf**, zero
  exclusions. `src/` carries only two small `#if defined(TARGET_DC)` branches
  (below); every *compat* fix lives in `dc/include/dc_prelude.h` as a
  force-include.
- **The harness works and is verified against real CDIs**, not asserted.
- **The image is 21,374,996 B** (text 6,318,568 / data 2,638,852 / bss
  12,415,508), ending at `0x8d472814`.
- **The image budget is 10,306,464 B** (corrected — see below), so
  **~11,068,532 B must still be shed.**

## The one paragraph that matters

`.text` (6,318,568) + `.data` (2,638,852) = **8,957,420 B.** Against the
corrected image budget of 10,306,464 B that leaves **1,349,044 B of headroom
for all of `.bss`** — which is 12,415,508 B today. Every plan written so far
targets `.bss`, and `.bss` must shrink by **89%** for the image to fit. Since
`-O0` is mandatory, `.text` cannot shrink; it can only be relocated, and the
only mechanism for that (MMU paging) came back **DEAD** — see below.

**Required cut: ~11,068,532 B** (was reported as 14,451,476; see the budget
correction below for why that was wrong in kind, not just magnitude).

## Budget corrected — ~3.4 MB found, none of it by shrinking anything

`kb/research-budget-premises.md` re-derived the budget and found the old one
**double-counted**: three "heap buckets" are not heap at all — they are `.bss`
already inside the image — and a fourth counts bytes twice.

| Input | Claimed | Corrected | Why |
|---|---:|---:|---|
| KOS baseline | −1,000,000 | **−262,144** | KOS/newlib/libstdc++ are *inside* our ELF (304,829 B in the map); only ~151 KB is additive |
| Bucket 9 audio | −700,000 | **−0** | not heap — `jaudio_NES` is 1,265,101 B of `.bss`, already in the image |
| Bucket 10 disc I/O | −384,000 | **−0** | not heap — `dc_dvd.c.o` `.bss` is 13,320 B; no read-ahead ring exists yet |
| Bucket 11 PVR staging | −384,000 | **−0** | not heap — `g_gx` is 334,764 B of `.bss` in `dc_gx.c.o` |
| Bucket 12 stacks | −131,072 | −65,536 | KOS's 64 KB kernel stack was already subtracted |
| **image budget** | **8,035,072** | **10,306,464** | |

State the fit as one inequality rather than two pools — splitting it is what
caused the error:

```
(image span) + (genuinely additive heap) ≤ 16,646,144
  image span today  21,374,996   (0x8c010000 → _end 0x8d472814)
  additive heap      4,839,680   (KOS 262,144 + arena 4,000,000
                                  + ARAM window 512,000 + threads 65,536)
  ⇒ over by          9,568,532
```

Bucket 6 (the 4,000,000 B `__osMalloc` arena) is **still unmeasured**, but
**≥1,294,497 B of it is provably dead** — remove the dead XFB/FIFO allocations
and the required cut falls to **9,774,035 B**.

## Why `.bss` is not free

**KOS's `mm_sbrk()` starts at the ELF `end` symbol. No MMU is enabled, no lazy
commit. Every `.bss` byte literally destroys a heap byte.** This is the fact
the whole size problem turns on, and it is why compression and debug-stripping
are worth exactly zero (`.bss` is `NOBITS` — there is nothing in the file to
compress).

## Standing constraint — the `-O0` directive

> "the optimizations cause problems and we cant use them without the port
> being broken"

**`-O1` / `-O2` / `-Os` / LTO are banned.** Not "risky" — banned, by user
decision. The armhf record is why: `-O2` gave a wild-pointer crash loop from
boot, `-O1` a hard SIGBUS on the intro train scene. Do not propose or benchmark
optimization as a size or speed lever; that argument has been had and retired.

**Codegen vs layout** is the operative distinction:

| Lever | Changes instruction selection? | Allowed |
|---|---|---|
| `-O1/-O2/-Os`, LTO, `-mrelax` | yes | **no** |
| `.bss` right-sizing, arena sizing | no | yes |
| Moving data/code to `/cd`, demand loading | no | yes |
| Linker script placement, code overlays | no | yes (MMU paging is DEAD — see below) |
| Offline asset conversion / decimation | no | yes |

## Boot status — failure fully explained

`harness/dc/smoke.sh` on the real CDI: **timeout, zero bytes of console
output.** Attributed by controlled experiment, not inference:

| image | `.bss` | end | result |
|---|---:|---|---|
| `selftest.cdi` (control) | 22,728 | `0x8c048948` | PASS 3.10 s |
| hello-world + 4.7 MB bss | 4,722,728 | `0x8c4c40a8` | PASS 3.08 s |
| hello-world + 21 MB bss | 21,022,728 | `0x8d44f888` | **FAIL, 0 bytes** |
| `OpenCrossing.cdi` | 12,415,508 | `0x8d472814` | **FAIL, 0 bytes** |

A stock KOS hello-world containing *nothing but* a big array fails identically
at the same image end. **The silence is size alone** — not a game fault, and
not the `dc_main.c` trampoline. Startup zeroing runs off physical memory before
`scif_init()`, so the guest never executes an instruction. There is no crash to
symbolise until the image fits. Corollary: the trampoline is still untested,
merely not implicated.

## RAM levers — status

1. **Asset destination arrays — 8,771,358 B (64.5% of `.bss`). THE lever, not
   yet implemented.** Confirmed to the byte by *three* independent methods
   (`mem-budget.md` §2 symbol attribution, the asset agent's loader replay, the
   build agent's `nm -S` sweep). These are `#ifdef TARGET_PC` placeholder
   arrays that `pc_assets.c` fills eagerly at boot. Fix is demand-loading into
   pooled storage: a **loader-only change, no codegen**. Everything else is a
   rounding error next to this.

   ⚠️ **Correction to an earlier framing.** This session claimed the 8.5 MB was
   "free PC scaffolding" that would vanish by reverting to the GameCube path.
   `kb/research-budget-premises.md` §6.2 says that is **probably FALSE as a RAM
   lever**: under the non-`TARGET_PC` branch those arrays become *initialised*
   data — the same resident bytes moved from `.bss` to `.data`, plus disc bytes.
   On a no-MMU sbrk machine that is neutral at best. It may still be true as a
   description of history. **The saving comes from demand-loading, not from
   flipping the define.** Also unverified: whether `src/data/**/assets/*.inc`
   even exist in the repo — a partial check suggested **only 4 `.inc` files
   exist repo-wide**, which if true means the non-`TARGET_PC` branch does not
   build at all and reverting is not an available option. One `ls` settles it.
2. **Resident REL blob — 16.56 MB peak. SOLVED, tool built and verified.**
   `dcasset pack` emits `assets.pak` (8,917,568 B) + a 51,104 B resident index,
   replacing the resident `foresta.rel` + `main.dol` (16,558,776 B). Round trip
   replays 16,365 references over 8,884,894 B with **zero mismatches**. Chunks
   are pre-byte-swapped offline (SH-4 never runs `do_swap`) and laid out in
   real load order — 82 backward reads, max reach 7,520 B, so an **8 KB window
   gives zero seeks**; one linear 8.9 MB read, 17.8 s at 500 KB/s. Also
   replaces `foresta.rel` on disc (−6.7 MB, no Yaz0 at boot). **Remaining work
   is the runtime loader in `pc_assets.c`** — and that same loader is what
   unlocks lever 1. See `kb/asset-pack.md`.
3. **Applied this session, −1,111,040 B** (measured delta equals the sum
   exactly): `prbuf` `sizeof(u32)`→`u16` −614,400 · `TEX_BUFFER_DATA_SIZE`
   `0x80000`→`0xC000` −475,136 · `TEX_BUFFER_BSS_SIZE` `0x4000`→`0x400`
   −15,360 · `TEXTURE_CACHE_LIST_SIZE` 1024→256 −6,144. All four are reversions
   of PC-port inflation back to **retail GameCube values**, so sufficiency is
   proven by the shipped product.
4. **Still on the ranked list, ~4.3 MB total** (from
   `kb/research-size-reduction.md`, measured against the real ELF + map):
   `.data` `src/data` tables to disc −1.94 MB (0.95 MB pointer-free today,
   0.99 MB needs a REL-style reloc pass) · `s_assets[]` name-string pool →
   disc index −0.89 MB · `audiomemory`/jaudio → AICA −0.65 MB · emu64
   `texture_buffer_data` → VRAM (partly taken in item 3) · actor overlay
   staging arenas → one shared union arena −0.46 MB · `pc_m_card` −0.28 ·
   `dc_gx` −0.24.
5. **Not yet costed: offline asset decimation.** The only lever that shrinks
   the destination arrays *themselves* rather than relocating them. Disc is
   5.3% full and the target is 640×480. `src/data/model` alone is 5,682,621 B
   of `.bss`. PLAN §1 already sanctions a documented "DC edition". **This is a
   product decision for the user, not an engineering one.**
6. **Not yet checked: source-level table dedup.** `--icf` is unavailable on SH,
   but `src/data` is generator output — hashing table contents and aliasing
   duplicates in `gen_runtime_assets.py` is a *generator* change, not codegen.
   Nobody has looked.

## Closed — do not re-propose any of these

- **`--gc-sections` is mandatory, not an optimization.** `DC_GC_SECTIONS=0`
  **does not link**: the decomp has genuinely undefined symbols whose
  referencing sections GC removes (`JKRTask::searchBlank()`, `vtable for
  JSUOutputStream`, `JSURandomOutputStream::getAvailable()/skip`) plus KOS's
  `__kos_romdisk`. Its recovery is already spent: 522,150 B (map-based,
  authoritative).
- **`--icf`** — no SH backend in gold, no ICF in `ld.bfd`, no SH port of `lld`.
- **SH GCC has no small-data model** — no `-G`/`-msdata` in `sh.opt`; the KOS
  script's `.sdata`/`.sbss` are inert, 0 bytes in the map.
- **`-g0` / strip saves exactly 0** — no debug section carries the `A` flag or
  appears in any `PT_LOAD`; `objcopy -O binary` never emitted it.
- **Compressing `1ST_READ.BIN` saves 0 RAM** — `.bss` is `NOBITS`.
- **AICA's 2 MB cannot hold a C array** — DMA-only over a 16-bit 25 MHz G2 bus.
  (Still open as a *paging backing store*; see Unfinished.)
- **emu64 is NOT an N64 emulator and there is no emulated RDRAM anywhere.**
  It is a GBI display-list interpreter emitting GX. `emu64.hpp:750`'s
  `u32 segments[16]` is 64 bytes of real GameCube pointers; `seg2k0()`
  bounds-checks `0x80000000..0x83000000` because that is GameCube MEM1, not
  because it is an emulated image's extent. Game logic is *ported* — `src/`
  carries the same TUs as the N64 decomp and `src/static/libultra/`
  reimplements the N64 OS API on Dolphin OS. Only genuinely emulated memory in
  the build is rspsim's 4 KB `DMEM[0x1000]`. Whole emu64 tree = 562,374 B of
  `.bss`. Verified independently twice. See `kb/research-n64-origin.md`.
- **`foresta.map`/`static.map` (5,402,023 B of disc) are droppable.** Only
  reader is `JUTException::queryMapAddress_single` on the `OSSetErrorHandler`
  path, which returns false outside `0x80000000..0x82FFFFFF` — no SH-4 address
  qualifies.
- **`-fno-builtin` breaks the link.** `m_select.c:936,993` then call a real
  `alloca` newlib does not provide, and there is no `-fbuiltin-alloca`.
  `kb/design-shelf-hazards.md` marked it "(VERIFIED)" as KOS convention; that
  was false for this image.
- **`-DTARGET_PC` is non-negotiable and must stay.** It means "not GameCube",
  not "PC": it guards the base port's little-endian correctness fixes
  (byte-wise texconv in `emu64.c`, swapped `u16` pair ordering in
  `sys_matrix.c`, overlap-safe `Jac_bcopy` in `sample.c`). `-DTARGET_DC` is
  added *alongside* it for genuinely DC-only branches.

## Research that landed at the end of this session

All three agents hit the session limit. Two wrote their documents; the third
was salvaged from its scratchpad by the main thread.

- **`kb/research-mmu-paging.md` — VERDICT: DEAD.** Read it before ever
  reconsidering MMU paging. The killer: **the MMU cannot create memory, and we
  have no backing store to page against.** On SH-4 the entire 29-bit physical
  space is already directly addressable with the MMU off via P1/P2, so the MMU
  buys only protection (don't need it) and oversubscription against a backing
  store (don't have one). The only store big enough for 8.5 MB is the CD-R at
  ~500 KB/s: one 4 KB page is **8.19 ms of transfer against a 33.3 ms frame
  budget — 24.6% of a frame per fault** — while the fault mechanism itself
  costs ~1.5–2.5 µs. **The backing store costs ~4,000× the fault.** And the
  comparison that settles it: `assets.pak` already pages the same bytes off the
  same CD, but at asset granularity, in load order, pre-swapped, with zero
  seeks — MMU paging would replace that with 4 KB faults at arbitrary
  instruction boundaries with no prefetch, batching, or load-order knowledge.
  *A strictly worse implementation of something the project is already
  building.* Four secondary findings each sink it independently: KOS's dynamic
  mapper forces every paged page **uncached**; TLB reach is 253,952 B against
  8.6 MB; KOS has **no eviction path at all**; and MMU-on makes store queues
  fault-prone on SH7750 silicon.
- **`kb/research-budget-premises.md`** — the corrected budget above. Two of six
  questions only partly answered; its §6 lists exactly what is missing and how
  to finish it. **Read §6 before starting any budget work.**
- **`kb/research-second-tier-memory.md` — salvaged fragment, not a real doc.**
  The agent died before writing. Recovered: a complete but **never-compiled,
  never-run** benchmark at `harness/dc/bench/bench_mem.c` that probes every
  main-RAM↔VRAM and main-RAM↔AICA path in both directions with checksum
  verification; plus uncited community bandwidth figures (SQ→RAM 495 MB/s,
  cacheline read+writeback 223.5 MB/s). **No VRAM *read* figure** — the
  original gap is still open. Now low priority: with MMU paging dead, VRAM and
  AICA are only interesting as destinations for specific buffers
  (`texture_buffer_data`, audio), not as general storage.

## Traps already paid for — do not re-discover these

- **`fsqrt`**: KOS `dc/fmath.h:109` defines `static inline float fsqrt(float)`;
  the decomp's `math64.h:34` `#define fsqrt(x) sqrtf(x)` rewrites KOS's
  *definition* into a static `sqrtf` that collides with newlib.
- **POSIX `link()` vs the decomp's `typedef struct link_ link`**, arriving via
  `<stdio.h>` → `<sys/stdio.h>` → `<unistd.h>`. A blanket `-Dlink=` does NOT
  work — it renames both sides. The prelude renames only the POSIX declaration,
  then gives the identifier back.
- **Guest `scif_flush()` permanently kills the Flycast console.** KOS's flush
  clears TEND and spins; Flycast never re-raises TEND on an idle TX FIFO; KOS
  latches `serial_enabled = 0`; a later crash then prints **nothing**. Bisected
  across 7 guest variants — raising baud is fine, the flush is the killer.
  Never call it.
- **KOS 2.3 assertion text** is `*** ASSERTION FAILURE ***` / capital-A
  `Assertion "x" failed`. The documented lowercase regex never matched, so a
  failed `assert()` only ever surfaced as a timeout.
- **`bash -lc` in the SDK image** re-runs `/etc/profile` and drops
  `/opt/toolchains/dc/sh-elf/bin`, so `sh-elf-addr2line` vanishes and every
  address silently symbolises to `??`. Use `bash -c`.
- **Sourcing `environ.sh` under `set -u`** exits 127 with nothing on stderr.
- **mkdcdisc padding**: default 740,083,145 B / 15.6 s vs `-N` 1,783,337 B /
  0.021 s. Use `-N` for every emulator run; `DC_CDI_PAD=1` only for burns and
  read-speed-realistic timing.
- **3900 object paths exceed `execve`'s `ARG_MAX`** — the Makefile uses make's
  `$(file …)` to build a linker response file.
- **Host has no BuildKit** — `DOCKER_BUILDKIT=0`, never pass `--progress`.
  `--platform linux/arm64` is not optional; without it an amd64 pull drops the
  build into qemu.

## Toolchain

`opencrossing-dc:sdk` in the local Docker daemon (do not rebuild — it is ~27
min cold): sh-elf GCC 15.2.0, newlib 4.6.0.20260123, binutils 2.45.1, KOS 2.3.0
(`1c6398f9`), kos-ports (`f4faacc4`), GLdc (`a1cd80a8`), mkdcdisc (`3c2ef63a`),
`-m4-single`, thread model kos. **char is SIGNED by default** on this build, so
`-fsigned-char` is belt-and-braces, not load-bearing.

```bash
bash dc/build-dc-image.sh        # build the image (idempotent)
bash dc/build-dc.sh              # HOST entry point -> ELF + unpadded CDI
DC_TARGET=objs bash dc/build-dc.sh
bash harness/dc/smoke.sh <cdi>   # boot in Flycast, assert on console
bash harness/dc/crash.sh <cdi>   # symbolise a fault
```

`dc/build-dc-docker.sh` runs **inside** the container and is not a host entry
point. Clean build ≈ 97 s for 3917 TUs + link + CDI at `-j4`.

## Next actions, in order

1. **Read whichever of the three `kb/research-*.md` files above exist.** They
   answer the viability questions; do not re-derive them.
2. **Measure bucket 6** if the premises agent did not. Cheapest win available.
3. **Implement the `pc_assets.c` runtime loader against `assets.pak`**, then
   demand-load the 8,771,358 B of destination arrays into pooled storage. This
   is the single largest lever and it is loader-only. Two rules from the pack
   author: **log window faults, never swallow them** (a regenerated
   `pc_assets.c` that reorders calls silently degrades to `fs_seek` + binary
   search — correct but minutes slower), and **do not delete `do_swap`** (a
   future regeneration with a swap conflict ships that chunk raw with the
   `PRESWAPPED` bit clear).
4. **Decide the `.text` question** — MMU paging, overlays, or accept that
   content must be cut. This is the fork in the road for the project.
5. Re-link, re-run `smoke.sh`, and expect a *real* crash to symbolise once the
   image fits — the first one will be informative.

**Be honest in reporting.** "Still N MB short with `-O0` mandatory" is a valid
and important result. If the levers do not close the gap, cutting content or
declaring a stock-16 MB build infeasible are the honest options; quietly
reopening the optimization question is not.

## Standing constraints

Stock 16 MB DC — the 32 MB mod must never become a requirement. No shaders, no
T&L, one texture unit. VMU ≈ 100 KB vs a ~456 KB GC save. CD-R ~500 KB/s, so
all disc I/O needs read-ahead. Game code stays `-O0`. Every optimization gets a
kill switch. **Never commit ROM material or built disc images** — no `.iso`/
`.gcm`/`.cdi`/`.gdi`/`.gci`. The user's ISO is at
`/Users/gabe/Documents/GitHub/OpenCrossing-Anbernic/harness/rom/Animal
Crossing.iso` (GAFE01 USA Rev 0, 1,459,978,240 B) — reference it, never copy it
in. `pc/` is reference material, not a build target. Agents must not run git;
the main thread commits. Branches: `main` = releases, `dev` = daily; never tag
dev. Emulator-first iteration (Flycast), hardware for truth; the dev console is
a known-good MIL-CD unit that boots burned CD-Rs.

## Caveat on everything in `kb/`

The first session's deliverables were written by agents whose **adversarial
verifiers all died**, so they are unreviewed. Treat their numbers as claims
until confirmed. Three have already been falsified by contact with the real
toolchain: `-fno-builtin` ("VERIFIED", breaks the link), the header-collision
scan (measured GCC 9.3/KOS `525cbda`, not our GCC 15.2/KOS 2.3, and missed both
collisions that actually bit us), and "no MMU" (true of KOS's default config,
false of the hardware).
