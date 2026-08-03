# The upstream PC port — what transfers, what does not

Reviewed 2026-08-02 against
<https://github.com/flyngmt/ACGC-PC-Port/releases> (v0.8-playtest through
v0.9.3-playtest, Mar–Aug 2024). `pc/` in this repo is a fork of that project
and is **reference material, never a build target** (CLAUDE.md §1).

The question asked was "is there anything to learn from this". Short answer:
**their headline performance work does not transfer, one of their bug fixes we
already have, and one of their defaults is a real divergence worth
understanding.** Details below so nobody re-reads the changelog.

## ❌ Their 1.5–2× framerate win does not transfer

v0.9.2-playtest advertises "big performance improvements" and "1.5x to 2x
framerate improvement for most users". The release notes attribute it to
**"generating shader variants"**.

That is a GPU-side, shader-compilation win. The PowerVR CLX2 is
**fixed-function with no shaders at all** (CLAUDE.md §1, hardware contract), so
there is nothing to port. It also would not help if there were: `kb/perf-dc.md`
measures this port's town frame as **58 % emu64/display-list traversal on the
SH-4** and only 26 % renderer. Their bottleneck was the GPU path; ours is the
CPU interpreting GBI commands.

**Do not re-open this.** A shader-variant cache has no meaning on this target.

## ✅ Their texture-cache fix — already inherited

v0.8.1-playtest lists "texture cache stale-data detection" alongside
"missing/incorrect textures". This looked like a direct hit on the
title-screen missing textures, because `dc_pvr_texture.c`'s own header warns
about exactly that failure mode: *"a paletted texture whose TLUT was not
resident at upload time decodes to a flat value and then stays cached for the
whole run, because evictions only happen under VRAM pressure — so a black CI
texture is permanent and silent."*

**We already have the fix.** The cache key (`key_hash`/`key_equal`,
`dc_pvr_texture.c:844`) includes `tlut_ptr` **and `tlut_hash`**, so a texture
re-bound with different palette contents produces a different key and is
re-uploaded. The FNV-1a content hashing was ported from `pc_gx_texture.c:51/:79`
with the same constants. Checked, not assumed.

## ⚠️ `fps_target` — a real divergence, and an unsettled question

| | upstream | this port |
|---|---|---|
| default | `6` — dynamic; the comment reads *"logic stays 100 % speed, render fps floats (60 cap)"* (`pc/src/pc_settings.c:30`) | `3` — a fixed 30 (`dc/src/dc_misc.c`) |

`src/graph.c:405-422` derives logic ticks per presented frame from this:

- fixed path (`case 3`): a constant **two** ticks per frame — correct only if
  30 fps is actually being presented.
- dynamic path (`== 6`): `ticks_per_visual = 60.0 / g_pc_fps_target`, so as the
  controller lowers the render target, graph runs proportionally **more** ticks
  and game logic stays at 60 Hz.

The town presents ~10 fps, so on the fixed path the game receives 20 logic
ticks per second instead of 60. The controller the dynamic path needs is
**already ported** — `dc_dynamic_fps_update()` in `dc_vi.c`, EMA α = 0.25,
clamped to `[10, 30]`, with the upward probe that stops the measurement
latching bistable. Nothing selects it.

**Tried on 2026-08-02 and reverted.** Setting it to 6 left the `[PERF]` speed
reading at 33–36 %, unchanged — because **"speed" in that line is `fps/30`, not
the logic rate**, so it could not have shown the win either way. In the same
window a human on a concurrent build reported the game "runs super slow", which
is what six logic ticks per presented frame would look like.

**To settle it, measure the LOGIC rate directly** — count `game_main` ticks per
wall second under `fps_target` 3 vs 6. If the dynamic path really holds 60
ticks/s where the fixed path gives 20, it is worth the presented frames. Do not
flip the default again without that number.

## Not evaluated, and why

- **Framerate independence / "unlocked FPS"** (v0.9.1). Delta-time conversions
  across "hundreds of files", which upstream itself warns may have missed
  conversions. Those edits live in `src/`, which §1 forbids editing, and this
  port is pinned to a 30 fps design target. Only interesting if the logic-rate
  question above resolves in favour of dynamic.
- **Borderless acres** (v0.9.2, on by default upstream). Removes acre
  transitions. On a target that is CPU-bound on display-list traversal, drawing
  *more* neighbouring acres is likely a net loss. Would need measuring, not
  assuming.
- **Gameplay fixes** (Resetti, mailbox black screens after town travel, weather
  inconsistency, money-rock randomisation, save-import corruption, NES高 scores,
  fireworks, insect animation rates). These are `src/`- and `pc/`-side game
  bugs. Whether this fork predates them is **not established** — no version
  marker was found in `pc/`. If a game-side bug here matches one of their
  changelog entries, that changelog is worth grepping before debugging it from
  scratch; `kb/issues.md` is the place to record such a match.
- **NES emulator, GBA connectivity, Memory Card A/B travel.** Out of scope; the
  NES CHR sheet is already a documented deliberate rejection in
  `dc_pvr_texture.c` (1024×256 I8, 262,144 texels — 512 KB of `.bss` for an
  unlockable minigame on a target 7 MB over budget).

## The standing rule this reinforces

Upstream optimises against a GPU and a modern CPU with codegen enabled. This
port has a fixed-function GPU, an SH-4 at 200 MHz, and `-O0` mandatory on
`src/`. **Their performance conclusions are not evidence about this target.**
Their *correctness* fixes are worth reading; their perf work generally is not.
