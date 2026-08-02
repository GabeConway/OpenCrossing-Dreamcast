# Session state — resume here

Updated 2026-08-01, end of the third execution session. This file is kept
**short on purpose**: it carries only what is true *right now*. Standing
knowledge lives in three companions, read on demand:

| file | read it when |
|---|---|
| `kb/levers.md` | planning any size/RAM work — the ranked ledger of what's left |
| `kb/closed.md` | **before proposing** any RAM/size/architecture idea — what is already dead and why |
| `kb/traps.md` | before touching the build, harness, or prelude |

`CLAUDE.md` is the index to everything else.

## Headline

**M0 and M1 are met. M2 is blocked on RAM. The port is not yet known to be
viable at stock 16 MB.**

- **3917 / 3917 translation units compile and link for sh-elf**, zero
  exclusions. `src/` carries only **four** small `#if defined(TARGET_DC)`
  branches; every *compat* fix lives in `dc/include/dc_prelude.h` as a
  force-include. That is the whole licence to touch `src/`.
- **The harness works and is verified against real CDIs**, not asserted.
- **The image does not boot, on size alone**, proven by controlled experiment.

## The one inequality

```
(image span) + (genuinely additive heap) ≤ 16,646,144
  image span today  21,374,068   (0x8c010000 → 0x8d472874)
  additive heap      3,545,184   (KOS 262,144 + arena 2,705,504
                                  + ARAM window 512,000 + threads 65,536)
  ⇒ over by          8,273,108
```

Sections: text 6,318,552 / data 2,638,852 / bss 12,415,508.

**Do not restate this as two pools** (an "image budget" vs a "heap budget").
Splitting it produced two wrong numbers already — 14,451,476 and then
11,068,532. `dc_mem_ledger.c` prints exactly this line as `MEMLEDGER FIT …`
from the linker symbols, and its compile-time check tests
`DC_HEAP_ADDITIVE ≤ DC_RAM_USABLE_BYTES` rather than summing every bucket (a
sum cannot detect a double-count).

Derived form, which is what the plan below is costed against:

```
usable RAM                                    16,646,144
  − additive heap                              3,545,184
  − .text 6,318,552 + .data 2,638,852          8,957,404
  ────────────────────────────────────────────────────────
  = .bss ceiling                                4,143,556
    .bss today                                 12,415,508  → shed 8,271,952
```

`.text` + `.data` = 8,957,404 B and neither can shrink — `-O0` is mandatory, so
`.text` can only be *relocated*. `.bss` must fall by **~67%**.

The one lever big enough is demand-loading the 8,771,358 B of asset destination
arrays (`kb/levers.md` L1) — that alone lands `.bss` at 3,644,150, under the
ceiling. **But the pool it loads into is additive heap, so it may be at most
~498,250 B.** That constraint drives the whole plan below.

## S1 IS DONE — the port has executed (2026-08-01)

**The Dreamcast port runs.** `DC_ASSET_STUB=1` shrinks every asset destination
array to one element; the image fits and boots in Flycast, and for the first
time in the project's history the platform layer has been observed working
rather than assumed to work.

```
MEMLEDGER FIT image_span=12375220 additive_heap=3545184 usable=16646144
              margin=725740 OK
```

Confirmed running, in this order, from one boot: `dc_main.c`'s trampoline · KOS
2.3 init and the serial console · maple enumeration (controller + 2 VMUs) ·
`dc_mem_ledger_init()` and `MEMLEDGER FIT` · `vid_set_mode` 640x480IL NTSC ·
the GX accumulator (`verts=8192 x 40B`) · iso9660 `/cd` mount · `ac_entry()` ·
`boot_main()` → `OSInit()` arena (0x8cbf8bc0–0x8ce8d420, 2642 KB) · `DVDInit` ·
the ARAM window · `PADInit` · `GXInit` · `AIInit` and the audio ring ·
`Na_InitAudio` (the jaudio heap sets up: `AUDIOHEAP SET ADDR 8c9d6e20h`) ·
`sound_initial()`'s 2.5 s wait · `initial_menu_init` · `dvderr_init` ·
`sound_initial2()` · `LoadStringTable` · `JW_Init2`.

**Where it stops, and why it is not a port bug:** the CDI is built from the ELF
alone, so `/cd` carries no game data. `JKRAramArchive::open()` mounts a
zero-byte `forest_1st.arc`, byte-swaps a garbage `num_file_entries`
(4,235,863,808) and walks off memory. Every stop before it is the same story —
`miss: /cd/audiorom.img`, `/cd/COPYDATE`, `/cd/static.str`. Getting further
needs disc content, which is the `tools/dcasset` track, not a platform fix.

Three things this cost, all now fixed and kept: `MEMLEDGER FIT` is printed from
`dc_mem_ledger_init()` (it used to print only from `dc_mem_report()`, which runs
when `main()` returns — the game never returns); `g_pc_verbose` defaults on
under `DC_ASSET_STUB` or `-DDC_VERBOSE`, because every `OSReport` in the game is
gated on it and a burned CD-R passes no argv, so without it a bring-up run is
blind; and `dc_main.c` skips `pc_assets_init()` under `DC_ASSET_STUB` so the
central table cannot memcpy full-size assets over one-element destinations.

How to rebuild it:

```bash
DC_ASSET_STUB=1 bash dc/build-dc.sh    # regenerates dc/build/stubsrc, then builds
bash harness/dc/smoke.sh dc/build/OpenCrossing.cdi --timeout 120
```

`tools/dcstub/make_stub_data.py` rewrites 2,535 TUs (16,317 arrays,
**8,716,158 B**) into `dc/build/stubsrc`, mirroring repo-relative paths;
`dc/Makefile` swaps those in per-TU. `src/` is not touched and nothing is
committed — this is a throwaway image, thrown away when S4 lands. Sections with
the stub: text 5,794,828 / data 2,638,852 / bss 3,939,828.

**The corollary in the next section is now discharged: the trampoline is
tested.** Everything below describes the unstubbed image, which is unchanged.

## Boot status — failure fully explained

`harness/dc/smoke.sh` on the real CDI: **timeout, zero bytes of console
output.** Attributed by controlled experiment, not inference:

| image | `.bss` | end | result |
|---|---:|---|---|
| `selftest.cdi` (control) | 22,728 | `0x8c048948` | PASS 3.10 s |
| hello-world + 4.7 MB bss | 4,722,728 | `0x8c4c40a8` | PASS 3.08 s |
| hello-world + 21 MB bss | 21,022,728 | `0x8d44f888` | **FAIL, 0 bytes** |
| `OpenCrossing.cdi` | 12,415,508 | `0x8d472874` | **FAIL, 0 bytes** |

A stock KOS hello-world containing *nothing but* a big array fails identically
at the same image end. **The silence is size alone** — not a game fault, and
not the `dc_main.c` trampoline. Startup zeroing runs off physical memory before
`scif_init()`, so the guest never executes an instruction. There is no crash to
symbolise until the image fits.

Corollary: the trampoline is still **untested**, merely not implicated.

## Toolchain

`opencrossing-dc:sdk` in the local Docker daemon — **do not rebuild, ~27 min
cold**. sh-elf GCC 15.2.0, newlib 4.6.0.20260123, binutils 2.45.1, KOS 2.3.0
(`1c6398f9`), kos-ports (`f4faacc4`), GLdc (`a1cd80a8`), mkdcdisc (`3c2ef63a`),
`-m4-single`, thread model kos.

```bash
bash dc/build-dc-image.sh        # build the SDK image (idempotent)
bash dc/build-dc.sh              # HOST entry point -> ELF + unpadded CDI
DC_TARGET=objs bash dc/build-dc.sh
bash harness/dc/smoke.sh <cdi>   # boot in Flycast, assert on console
bash harness/dc/crash.sh <cdi>   # symbolise a fault
```

`dc/build-dc-docker.sh` runs **inside** the container and is not a host entry
point. Clean build ≈ 97 s for 3917 TUs + link + CDI at `-j4`. Details:
`BUILDING-DC.md`. Gotchas: `kb/traps.md`.

## Next actions — the agreed plan

**User chose this sequence on 2026-08-01 (S1 → S4, in order).** Do not
re-litigate the ordering; execute it. The reasoning behind each step is below
so a fresh context does not have to re-derive it.

### The two findings that shaped this plan

1. **`.text` overlays (`kb/levers.md` L4) are NOT needed.** The gap closes
   without touching `.text`. L4 was previously written up as "the fork in the
   road for the project" — that framing was wrong. Do not spend a session on
   the ScummVM `R_SH_DIR32` loader unless the arithmetic below stops holding.
2. **The asset pool is the binding constraint, not the arrays.** L1 removes
   8,771,358 B of `.bss` (→ 3,644,150), but the pool it loads *into* is
   additive heap. Solve the inequality for it: **the pool can be at most
   ~498,250 B** unless something else also moves. That is uncomfortably tight
   for streaming 8.9 MB of assets, which is why S3 comes before S4.

### Complication to budget for in S4

L1 is billed as "loader-only, no codegen". True, but it understates the work.
The destination arrays are referenced **by address** from initialised `.data`:

```c
Vtx glider_v[0xB0 / sizeof(Vtx)];                          /* .bss dest   */
Gfx glider_model[] = { … gsSPVertex(glider_v, 11, 0) … };  /* .data, baked ptr */
```

Pooling the storage means **fixing up every such reference at load time** —
`dcasset`'s round trip already replays **16,365** of them, so the tool has the
data, but the loader must apply relocations, not just `memcpy`.

### The structural risk S1 existed to kill — RETIRED, S1 killed it

*(Kept for the record. This was true until 2026-08-01; the section at the top of
this file is what replaced it.)*

**Zero lines of this port have ever executed.** Not on hardware, not in
Flycast. The boot failure is size alone, so `dc_main.c`'s trampoline, KOS init,
the platform layer, the GX stubs and `dc_mem_ledger.c`'s new `MEMLEDGER FIT`
line have never run once. Every RAM estimate assumes a platform layer nobody
has observed working. If S4 lands after a week and *then* the trampoline turns
out to be broken, the two failures are tangled and hard to attribute.

---

### S1. Stub the assets and BOOT IT. ✅ DONE 2026-08-01 — see the section above.

Landed as `tools/dcstub/make_stub_data.py` + `DC_ASSET_STUB=1`. The image boots
and reaches `JW_Init2`; it stops on an empty `/cd`, not on a platform fault.
**⭐ Start at S2.** The original write-up follows, unchanged, because it is the
argument for why the step was worth taking.


Build a **throwaway** image with the destination arrays sized `[1]`: a
`DC_ASSET_STUB` build mode that rewrites generator output into a scratch tree.
**No `src/` edits, nothing committed to the real tree** — `src/data/**/*.c` is
output of `pc/tools/gen_runtime_assets.py`, so regenerating small is a
*generator* change, legal under the `-O0` rule.

`.bss` → ~3,644,150, under the 4,143,556 ceiling with ~500 KB spare. **The
image fits and should boot.**

The game renders garbage the moment it touches an asset. That is expected and
fine. What S1 buys is the first execution of the trampoline, KOS init, the
console path, `MEMLEDGER FIT`, and `crash.sh` symbolising a real fault —
surfacing every platform-layer bug *now*, separately from the loader.

Cost: small. Buys: the largest available reduction in unknown-unknowns, plus
the first end-to-end validation of the harness. Throwaway once S4 lands.

### S2. Measure L6 — generator table dedup.

`src/data` is generator output; hashing table contents and aliasing duplicates
in `gen_runtime_assets.py` is a generator change, not codegen. **Nobody has
looked.** Could be 0, could be megabytes.

Cost: small — a host-side hash pass over the generated tables gives the number
without changing the build. Worth doing purely because the answer is cheap and
currently unknown.

### S3. Bank the independent savings — `kb/levers.md` L3, ~4.3 MB.

Six measured, mutually independent moves. Does **not** close the gap alone
(still ~3.97 MB over), but it is what turns S4's ~498 KB pool into a
comfortable ~4.8 MB one. Genuinely parallelizable; each item is separable and
independently measurable. Low risk — relocations of known-size objects.

### S4. Build the loader — `kb/levers.md` L1 + L2.

`pc_assets.c` against `assets.pak`, with relocation fixups for the 16,365
references. Contract: `kb/asset-pack.md`. The biggest single piece of work left
in the project, now attempted against a platform layer that S1 proved runs and
with a pool S3 made generous.

---

**Be honest in reporting.** "Still N MB short with `-O0` mandatory" is a valid
and important result. If the levers do not close the gap, cutting content
(`kb/levers.md` L5 — the user's call, not engineering's) or declaring a
stock-16 MB build infeasible are the honest options; quietly reopening the
optimization question is not.

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
