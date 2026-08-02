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

`.text` + `.data` = 8,957,404 B and neither can shrink — `-O0` is mandatory, so
`.text` can only be *relocated*. That leaves **`.bss` at ~4.14 MB maximum**
against 12,415,508 B today: it must fall by **~67%**.

The one lever big enough is demand-loading the 8,771,358 B of asset destination
arrays (`kb/levers.md` L1). Nothing else is close.

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

## Next actions

The gap is 8,273,108 B and one lever covers most of it. Everything below is
downstream of picking a path — see the options writeup in the session summary
or `kb/levers.md`.

1. **Implement the `pc_assets.c` runtime loader against `assets.pak`**, then
   demand-load the 8,771,358 B of destination arrays into pooled storage.
   Loader-only, no codegen. `kb/levers.md` L1 + L2, contract in
   `kb/asset-pack.md`. **This is the critical path.**
2. **Take `kb/levers.md` L3** (~4.3 MB of measured, independent `.bss`/`.data`
   moves) — needed *in addition* to L1, since L1 alone leaves `.bss` above the
   ~4.14 MB ceiling only if the pool is generous.
3. **Decide the `.text` question** (`kb/levers.md` L4). MMU paging is dead, so
   the live options are ScummVM-style code overlays, asset decimation, or
   accepting that content must be cut. **The decimation/cuts branch is the
   user's call, not an engineering one.**
4. **Re-link, re-run `smoke.sh`.** Expect a *real* crash to symbolise once the
   image fits — the first one will be informative, and it is also the first
   test the trampoline has ever had.

**Be honest in reporting.** "Still N MB short with `-O0` mandatory" is a valid
and important result.

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
