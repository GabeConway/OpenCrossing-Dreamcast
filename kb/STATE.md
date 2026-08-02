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

  image span        18,997,600   measured 2026-08-02, clean full-size rebuild
                                 text 5,749,944 / data 2,638,872 / bss 10,837,376
                                 _end 0x8d265f60
  additive heap      3,079,648   KOS 262,144 + arena 1,900,000
                                 + ARAM window 851,968 + threads 65,536
  ⇒ over by          5,431,104
```

At the *policy* knobs (arena 2,705,504 + ARAM 1,048,576) it is over by
6,433,216. Both supersede the 6,999,924 that older docs quote.

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
- **The vertex/model half is still unmeasured.** `GXSetArray` recorded zero
  hits; emu64 dereferences `Gfx`/`Vtx` inside `src/`, where there is no seam.
- **The framebuffer probe is attributed, not fixed.** `PVR_FB_R_SOF1` reads
  `0x000E7480`: the display scans out 947,840 B into VRAM, and `vram_s` has
  never been the displayed surface. `FBNONZERO` is the assertion to trust; a
  hash cannot tell black from wrong-address.

## ⭐ NEXT ACTIONS (2026-08-02) — ranked, do these in order

The S1→S5 plan in `kb/plan-stages.md` is still the RAM strategy and is not
superseded. These are the concrete next moves now that pixels exist.

### N1. Get the town to draw. [reframed 2026-08-02 — the texture half is measured, the vertex half is not]

The keep-list-by-hand plan is dead: the title demo's acres and animals are
named by index and profile ID, so there is no static list to extend from.
`DC_ASSET_CENSUS=1` replaces it and already answered the texture side (50
symbols, 111,136 B — see "Latest measurements" above). Two follow-ups, in order:

1. **Census the vertex/model side.** `GXSetArray` sees nothing because emu64
   walks `Gfx`/`Vtx` pointers inside `src/`. Options, cheapest first: census
   the `Gfx` pointer at the emu64→GX boundary that `dc_gx.c` already sees
   (`dc_gx.c:673`'s indexed-fetch path documents what does arrive); or hook
   `pc_load_asset`'s stub redirect; or accept a `#if defined(TARGET_DC)`
   branch — but that spends one of the four licences in `src/` and needs a
   reason better than instrumentation.
2. **Then extend `DC_STUB_KEEP` from the measured list, not by guessing.**
   111 KB of textures fits trivially; the models will not, and *that* boundary
   is the S4 pool-sizing measurement N1 was always after.

### N2. STILL OPEN, one step from done. [blocks all unattended visual work]

Attributed, not fixed. VRAM holds content (20 of 128 blocks by the end of a
run) and the display scans out from offset 947,840, so the old `vram_s` read
was looking at the wrong place — but `0xA5000000 + SOF1` still reads zero.
Next: try the 64-bit VRAM window `0xA4000000 + SOF1`, and hash the hot blocks
`FBSWEEP` names (12, 14, 20, 23) to locate the image directly. Fallback is
`pvr_scene_begin_txr()` into a guest-owned texture. Full evidence in
`kb/state-log.md`. **One build and one run should close it.**

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
