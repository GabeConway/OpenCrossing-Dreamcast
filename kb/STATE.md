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
- **The image budget is 8,035,072 B**, so **13,339,924 B must still be shed.**

## The one paragraph that matters

`.text` (6,318,568) + `.data` (2,638,852) = **8,957,420 B, which already
exceeds the 8,035,072 B budget with `.bss` at exactly zero.** Subtracting
`.text` alone leaves **1,716,504 B for all of `.data` + `.bss` combined**;
they are 15,054,360 B today. Every plan written so far targets `.bss`, and
**even deleting all of `.bss` does not make the image fit.** Since `-O0` is
mandatory, `.text` cannot shrink — it can only be *relocated*.

So viability rests on two unanswered questions:

1. **Is the 7.61 MB heap budget real?** Bucket 6 is 4 MB that was **never
   measured** — `jsyswrap.cpp:547` sets the game heap to
   `JKRHeap_getFreeSize(systemHeap) - 0x10000`, i.e. the port hands the game
   whatever is left rather than sizing to need. If the true peak is 1.5 MB,
   the image budget grows by ~2.5 MB. **Cheapest possible win in the project.**
2. **Can `.text` leave RAM?** Only two candidate mechanisms: SH-4 MMU demand
   paging (KOS supports it — see below) or ScummVM-style code overlays.

Research on both was in flight when the session ended; see "Unfinished" below.

## Why the budget is what it is

**KOS's `mm_sbrk()` starts at the ELF `end` symbol. No MMU by default, no lazy
commit. Every `.bss` byte literally destroys a heap byte.** This is the fact
the whole size problem turns on, and it is why compression and debug-stripping
are worth exactly zero (`.bss` is `NOBITS` — there is nothing in the file to
compress).

Budget = 16 MB − 7.61 MB heap (`dc/include/dc_mem_budget.h` buckets 6–12) −
~1 MB KOS = 8,035,072 B. Both subtrahends are unverified; see question 1.

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
| Linker script placement, overlays, MMU paging | no | yes |
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
   arrays that `pc_assets.c` fills eagerly at boot — **scaffolding the PC port
   added**, not something the game needs; the GameCube original read straight
   from the REL. Fix is demand-loading into pooled storage: a **loader-only
   change, no codegen**. Everything else is a rounding error next to this.
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

## Unfinished — three research agents were stopped mid-flight

Each was told to dump partial findings before stopping. **Check whether these
files exist and what state they are in — they may be complete, partial, or
absent:**

- `kb/research-mmu-paging.md` — **the SH-4 has an MMU and KOS supports it.**
  Verified present in our image:
  `/opt/toolchains/dc/kos/kernel/arch/dreamcast/include/arch/mmu.h` exposes a
  two-level sparse page table, `mmu_page_map`, `mmu_page_map_static`, and
  crucially **`mmu_map_set_callback()`** — a fault-handler hook returning a
  `mmupage_t*` for a faulting virtual page, i.e. demand paging. KOS's own
  header text: *"a few very interesting things that this functionality could be
  used for (like mapping large files into memory that wouldn't otherwise
  fit)"*. `kb/research-size-reduction.md`'s "no MMU" is true of KOS's default
  config, **false of the hardware.** Open: is the fault path complete enough;
  page size (64 UTLB × 4 KB = only 256 KB reach, but SH-4 supports 64 KB and
  1 MB pages); TLB-miss cost at 200 MHz; whether store queues and PVR DMA
  survive MMU-on; **and whether Flycast emulates the UTLB at all** — if it does
  not, we cannot iterate in the emulator and that alone may be decisive.
- `kb/research-second-tier-memory.md` — VRAM (8 MB, maybe ~4 MB spare) and
  AICA (2 MB, ~40 MB/s over G2) reconsidered as a *paging backing store* rather
  than as addressable memory, which is the framing that dodges the
  disqualification above. CD-R at ~500 KB/s is ~8 ms per 4 KB page and is
  almost certainly unusable as primary swap. A benchmark source
  (`bench_mem2.c`) for the missing SH-4↔VRAM bandwidth figure was being written
  — **check whether it was ever actually run before trusting any number.**
- `kb/research-budget-premises.md` — bucket 6 (see question 1 above), an audit
  of `dc_mem_budget.h` buckets 6–12, the real KOS+GLdc baseline, and a `.data`
  vs `.rodata` audit.

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
