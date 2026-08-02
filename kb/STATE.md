# Session state — resume here

Updated 2026-08-02. This file is **short on purpose**: only what is true *right
now*, plus what to do next. Everything else is one hop away.

| file | read it when |
|---|---|
| `kb/state-log.md` | you need the evidence — what was observed running, when, and what it cost |
| `kb/heap-two-pools.md` | **before touching `DC_ARENA_BYTES` / `DC_ARAM_WINDOW`** or anything that allocates at boot |
| `kb/plan-stages.md` | the agreed S1→S5 RAM plan and the reasoning behind each step |
| `kb/levers.md` | planning any size/RAM work — the ranked ledger of what's left |
| `kb/closed.md` | **before proposing** any RAM/size/architecture idea — what is already dead and why |
| `kb/traps.md` | before touching the build, harness, prelude, or instrumentation |

`CLAUDE.md` is the index to everything else.

## Where the port is

**M0 and M1 are met. M2 has first pixels and is not complete.** A
`DC_ASSET_STUB` image boots in Flycast, runs the game loop at 29.3 FPS, reaches
the title-demo scene and draws the Animal Crossing title overlay — "PRESS
START" and the copyright line — through a real PowerVR backend
(`dc/src/dc_pvr.c` + `dc_pvr_texture.c`), submitting ~558,000 triangles per run
with zero drops. The town behind the logo is black because only 53,792 B of
real assets are in that image; that is the S4 loader's job, not a renderer bug.

- **3917 / 3917 translation units compile and link for sh-elf**, zero
  exclusions. `src/` carries only **four** `#if defined(TARGET_DC)` branches;
  every compat fix lives in `dc/include/dc_prelude.h` as a force-include.
- **The harness works and is verified against real CDIs**, not asserted.
- **The full image still does not fit. That is the only thing between here and
  a playable build** — the renderer, the platform layer and the boot path are
  all observed working.

The build that renders:

```bash
DC_DISC_ROOT=~/.cache/oc-dc-discroot DC_ASSET_STUB=1 \
  DC_ARAM_WINDOW=851968 DC_ARENA_BYTES=1900000 \
  bash dc/build-dc.sh
bash harness/dc/smoke.sh dc/build/OpenCrossing.cdi --timeout 180
```

⚠️ A game smoke run **always** exits 1 with `status=exited_early` — the game
never returns, so the end-marker checks cannot pass. The console log is the
artefact. See `kb/traps.md`.

## The one inequality

State the fit as **one inequality, never two pools**. Splitting it into an
"image budget" and a "heap budget" has already produced two wrong numbers
(14,451,476 and 11,068,532).

```
(image span) + (genuinely additive heap) ≤ 16,646,144

  image span        18,988,416   measured 2026-08-02 after P7, clean rebuild
                                 text 5,804,776 / data 2,337,976 / bss 10,837,376
                                 _end 0x8d22bd80
  additive heap      3,079,648   KOS 262,144 + arena 1,900,000
                                 + ARAM window 851,968 + threads 65,536
  ⇒ over by          5,421,920
```

At the *policy* knobs (arena 2,705,504 + ARAM 1,048,576) it is over by
6,424,032. Both supersede the 6,999,924 that older docs quote.

⚠️ **Correction, 2026-08-02:** an earlier version of this block said the
pre-P7 span was 18,997,600 and the gap 5,431,104. That was an arithmetic slip
— `0x8d265f60 - 0x8c010000` is **19,226,464**, which is what the older docs
said all along. The `size` "dec" column is not the span: it omits inter-section
alignment and it counts `.ocram`, which lives at `0x7c001000` and is not in the
image at all. **Take the span from `_end` minus `0x8c010000`, never from
`dec`.**

`.text` + `.data` = 8,388,816 B and neither can shrink — `-O0` is mandatory, so
`.text` can only be *relocated*. The lever big enough is demand-loading the
8,771,358 B of asset destination arrays (`kb/levers.md` L1), **but the pool it
loads into is additive heap**, which is what makes S4's pool size the binding
constraint. `dc_mem_ledger.c` prints this line at boot as `MEMLEDGER FIT …`.

⚠️ **Measure only against a clean rebuild.** `dc/build/flags.stamp` now forces
one when flags change; before that fix a stale `dc_main.c.o` made a non-stub
ELF read 356,776 B too small.

⚠️ The ARAM window's floor is **851,968** — `forest_1st.arc` arrives as one
851,744 B transfer and a smaller window drops the whole archive. It grew from
512,000 on 2026-08-01 and that is a debt: PLAN §3.1's disc-backed LRU pays back
536,576 B of the gap.

## Latest measurements (2026-08-02) — full narrative in `kb/state-log.md`

- **Arena, first real measurement.** `DC_ARENA_PROBE=<frames>` reports the
  game's own allocator. At the title screen: **used 256,192 B of a 1,412,704 B
  zelda arena**, inside a 1,900,000 B knob, while libc had taken 2,666,496 B
  from sbrk. The arena is not where the pressure is. **Title scene only** — a
  loaded town is unmeasured, so this licenses a smaller *bring-up* arena, not a
  smaller shipping one.
- **Asset working set, textures.** `DC_ASSET_CENSUS=1` +
  `tools/dcstub/census_resolve.py`: the title screen touches **50 symbols /
  111,136 B** of real texture bytes — the logo glyphs, all seven `obj_train1_t*`
  textures, `grl_1_*`, and the animals' eye/mouth TA textures — against the
  4.6 MB of texture destinations the image keeps in `.bss`. Strongest evidence
  yet for `kb/research-creative-ram.md` T1.
- **The vertex/model half is measured: the whole title-screen working set is
  93,312 B** — 30,688 B of models (58 `gsSPVertex` batches, 18,720 B actually
  read) plus 62,624 B of textures and palettes, against **8,771,358 B** of asset
  destination arrays in `.bss`. `GXSetArray` is **dead in this game**, not just
  quiet: its only call site anywhere in `src/` is `GXInit.c:252`'s own reset
  loop. The seam that works is `OSs16tof32()`, whose only three calls in the
  emu64 TU are the three components of a source vertex; `dc/include/dc_census_vtx.h`
  wraps it and `dc/Makefile` force-includes it into that one TU.
- **The framebuffer probe is attributed, not fixed.** `PVR_FB_R_SOF1` reads
  `0x000E7480`: the display scans out 947,840 B into VRAM, and `vram_s` has
  never been the displayed surface. `FBNONZERO` is the assertion to trust; a
  hash cannot tell black from wrong-address.

## ⭐ PARKED 2026-08-02 — read this first if you are a fresh context

**The frontier is not a crash and not a stub: it is an unpressed button.**
`kb/boot-blockers.md` (read it — it is the ranked list of what the game reaches
next) traced the title demo to `aAL_game_start_wait`
(`src/actor/ac_animal_logo.c:245`), looping forever waiting for START or A.
Every other gate on `ac_animal_logo.c:268-273` is already open. Nothing in the
harness can press a button, so nothing past the title has ever been reached in
CI. That is item 4 on its list and roughly 30 lines of work.

Its other cheap wins, both measured: **85 % of every console log** is jaudio's
`SendStart::Mesg Full Queue` via `OSReport` (`dc/src/dc_os.c:606`, ~20 lines to
rate-limit), and **`OSGetSoundMode()` returns 0 = mono** (`dc_stubs.c:118`),
which silently locks the game to mono and contradicts the audio plan of record
— a one-line fix.

### Work that was in flight when this was parked

Five agents were running in git worktrees under `.claude/worktrees/`. Each
worktree holds uncommitted changes; **merge by path, not by `git diff`** — at
least one worktree had a stale HEAD, so its diff is enormous and meaningless.
Verify every merged change with a clean rebuild in the main tree before
believing its numbers; P7's did not reproduce exactly.

| worktree | task | owns |
|---|---|---|
| `agent-ab7b5a9310ecbe98d` | ARAM disc-backed LRU (PLAN §3.1) | `dc/src/dc_aram.c`, `dc_dvd.c` |
| `agent-a027be752134b705b` | P7 — ✅ **already merged** (`528900a`), safe to delete | — |

The ARAM agent was told to default its kill switch in its own header because
`dc/Makefile` was owned by another agent; **it owes a Makefile knob** — look for
it in its report or its header's `#ifndef`.

### The number the plan was waiting on — answered

**93,312 B.** A title-screen-complete build does not need S4. Extending
`DC_STUB_KEEP` with the censused models and textures is the next concrete step
(N1 item 2 below).

## Ranked next actions (2026-08-02) — the list before parking

The S1→S5 plan in `kb/plan-stages.md` is still the RAM strategy and is not
superseded. These are the concrete next moves now that pixels exist.

### N1. Get the town to draw. [reframed 2026-08-02 — the texture half is measured, the vertex half is not]

The keep-list-by-hand plan is dead: the title demo's acres and animals are
named by index and profile ID, so there is no static list to extend from.
`DC_ASSET_CENSUS=1` replaces it and already answered the texture side (50
symbols, 111,136 B — see "Latest measurements" above). Two follow-ups, in order:

1. ✅ **The vertex side is censused** — 93,312 B for the whole title screen, no
   `src/` edit needed. See "Latest measurements".
2. **Extend `DC_STUB_KEEP` from the measured list.** 93 KB fits trivially, so a
   **title-screen-complete build should land without S4**. The census names the
   models: `boy_1_v`/`grl_1_v`, `mnk_1_v`, `wol_1_v`, `dog_1_v`, the three
   `obj_train*_v`, five `logo_us_*_v`, `ef_hanabira01_00_v`, `ef_shadow_out_v`.
   ⚠️ Animal species attribution is **not decidable on a stub image** — every
   `xxx_1_v` is 16 B apart and all species share a rig, so 4 of 58 batches are
   ambiguous and some "certain" animal hits may be aliases. Keeping the named
   models gives them real sizes and spacing, which makes the join exact: do that
   first, then re-run the census to confirm before trusting the list.
3. Then re-run on a **town** scene — that is what actually sizes S4's pool, and
   the batch table caps at 1,024 (`full=0` on the title screen; a town run must
   check that counter).

### N2. ✅ DONE 2026-08-02 — the unattended visual gate works.

```bash
bash harness/dc/smoke.sh dc/build/OpenCrossing.cdi --fb-golden 25789d43
```

`fb_saw_pixels` and `fb_golden` come back in the JSON. The golden is the stub
title screen at `DC_ARAM_WINDOW=851968 DC_ARENA_BYTES=1900000`;
`FBNONZERO 13711 of 307200`, reproduced across four runs. The decoded 16×12
thumbnail shows two centred text bands in the lower third over black — "PRESS
START" above the copyright line, which is what a human reported seeing.

**`--fb-writeback` is required, not optional**, and my earlier note in this file
saying otherwise was wrong: without it every candidate surface reads zero, with
it the scanout surface reads real pixels. `0xA5000000 + FB_R_SOF1` was the right
address all along; the 64-bit-aperture hypothesis was wrong and Flycast's
32-bit aperture merely mirrors every block at +4 MB. The frame-rate cost of the
flag is **unmeasured** — the old 24.8 → 16.8 FPS did not reproduce and no
controlled pair exists.

### N2b. Wire the game's save path to the VMU. [the backend is real; nothing calls it]

`dc/src/dc_card.c` is a working KOS `vmufs`/`vmu_pkg` backend, proven in Flycast
and re-verified host-side out of the flash image. **But the game never calls
it:** `pc/src/pc_m_card.c` does its I/O with `<stdio.h>` against the relative
path `save/card_a/DobutsunomoriP_MURA.gci`, so of the 29 `CARD*` entry points
only `CARDInit()` is on its path and `[PC] No save file found` is a failed
`stat()`. `pc_card_scan_for_gci()` deliberately still returns 0 — returning a
path would make the game print "found" and then fail to `fopen` it.

The designed fix is `kb/save-plan.md` §7.8: a KOS `vfs_handler_t` mounted at
`/dcsave` plus an `fs_chdir()` from `dc_main.c`, committing to the VMU on the
`->rename` callback — which is exactly `pc_save_write_gci_ex`'s last step, so
it is a free atomic commit point with no edit to `pc/`.

**Measured and load-bearing:** a VMU block costs **84.6 ms** to write
(`write ≈ 0.678 s + 84.6 ms/block`, within 1.3% of the KOS-source ceiling), so a
150-block save is **13.4 s**. Incremental writes are mandatory and the shipped
chunking does *not* deliver them — `vmufs_write()` rewrites the whole file. The
format is byte-stable; the writer needs a block-diffing pass. Deflate-6 on SH-4
is 295,910 B → 99,657 B in 0.129 s, i.e. compression is free next to the flash
cost. ⚠️ Every compression ratio so far is against **synthetic** data; a real
`.gci` is the top open item.

### N3. Correct the TEV mapping. [the logo renders; is it renders *right*?]

`dc_pvr.c` implements exactly one TEV configuration — modulate texture by
rasterised colour — against the 101 in `kb/tev-map.md`. Konst-colour and
multi-stage configs currently come out untinted. Now that something is on
screen, this is measurable for the first time: instrument which of the 101
configs the title screen and the field actually request, and implement by
frequency rather than by enum order.

### N4. Measure bucket 6 properly. [PARTLY DONE 2026-08-02 — title scene measured, gameplay is not]

`DC_ARENA_PROBE=60` reports the game's own allocator every 60 presented frames.
At the title screen: **used 256,192 B of a 1,412,704 B zelda arena**, inside a
1,900,000 B arena knob, while libc had taken 2,666,496 B. No bisect needed and
no arena-side OOM is reachable from here.

What is left is the part that decides the shipping number: **the same probe on
a scene that has a town loaded**, which does not exist yet. Until it does, do
not cut `DC_MAIN_MEMORY_SIZE` on the strength of the title figure — cut it for
bring-up images if libc needs the room, and re-run the probe the moment S4
loads real field data.

### N5. Then S4 — the asset loader. [unchanged, still the critical path]

`kb/plan-stages.md` S4 still applies in full — read it before starting. Two
things changed since it was written: the pool must be sized against the
**libc** side of the split, not treated as a free-floating extent
(`kb/heap-two-pools.md`); and the S3 remainder is smaller than billed — P6
measured −598,424 B, not −821,569.

### Also worth knowing

- `SendStart::Mesg Full Queue` spams the console ~1,000 times per run. It is
  jaudio, it is not fatal, and it makes logs hard to read. Worth silencing.
- The ARAM window still thrashes (`rebases=13` mounting `forest_2nd.arc`).
  PLAN §3.1's disc-backed LRU is still owed and is now the thing standing
  between the game and real archive content.


## Toolchain

`opencrossing-dc:sdk` in the local Docker daemon — **do not rebuild, ~27 min
cold**. sh-elf GCC 15.2.0, newlib 4.6.0.20260123, binutils 2.45.1, KOS 2.3.0
(`1c6398f9`), kos-ports (`f4faacc4`), GLdc (`a1cd80a8`), mkdcdisc (`3c2ef63a`),
`-m4-single`, thread model kos. Clean build ≈ 97 s for 3917 TUs + link + CDI at
`-j4`. Entry points, every env knob and the flag assembly: `BUILDING-DC.md`.
Gotchas: `kb/traps.md`.

## Standing constraints

`CLAUDE.md` §1 is authoritative and must not be restated differently here.
The short version: stock 16 MB (the 32 MB mod must never become a requirement),
`src/` at `-O0` with codegen flags banned, never edit `src/`, never commit ROM
material or disc images, every optimization gets a kill switch, agents do not
run git — the main thread commits.

**Be honest in reporting.** "Still N MB short with `-O0` mandatory" is a valid
and important result. If the levers do not close the gap, cutting content
(`kb/levers.md` L5 — the user's call, not engineering's) or declaring a
stock-16 MB build infeasible are the honest options; quietly reopening the
optimization question is not.
