# AC decomp / port ecosystem research (2026-08-01)

Compiled 2026-08-01 from web research + GitHub API. Facts that shaped PLAN.md.

## ac-decomp (the foundation)

- https://github.com/ACreTeam/ac-decomp — **100.00% decompiled, 100.00%
  linked** (decomp.dev), milestone announced June 2025. Target **GAFE01_00
  USA Rev 0**. CC0-1.0, no assets/asm in repo. Built with decomp-toolkit +
  mwcc. Fully shiftable/moddable. Sister: afe-decomp (e+ JP).

## flyngmt/ACGC-PC-Port (upstream port)

- https://github.com/flyngmt/ACGC-PC-Port — created 2026-03-09, very active
  (pushed 2026-08-01). ~1.4k stars. 3-tier model: game (N64 DLs) → emu64
  (kept) → GX → **replaced with GL 3.3**. x86 32-bit only.
- Release line: v0.8 (2026-03-13) → v0.9.0 (2026-04-11, **NES via fixNES**,
  Kapp'n island) → v0.9.1 (2026-05-30, **unlocked FPS** with Cuyler36) →
  v0.9.2 (2026-07-10, shader-variant perf).
- **-O2 fix: commit `4f428276` 2026-07-10 "Compile everything at -O2"** —
  `-fno-strict-aliasing -fwrapv` neutralize type-punning + signed-overflow UB
  ("the decomp UB classes … that broke plain -O2", their CMakeLists comment).
  x86 only — does NOT cover the strict-alignment class ARM hit at -O1
  (unaligned LDRD/VFP SIGBUS); SH-4 is also strict-alignment. Necessary, not
  sufficient, for DC. See PLAN §3.2.
- **2026-08-01: large wave of delta-time commits** ("Fix fishing dt", NPC
  schedules, event/animation timing…) — frame-rate-independence follow-up to
  the unlocked-FPS work. Directly reusable for a 30 fps DC target: track and
  cherry-pick.
- Assets: no extraction step upstream — runtime parse of user's ISO in
  `rom/` (GCM FST, Yaz0), ~2500 assets loaded from DOL/REL offsets
  (`gen_runtime_assets.py`). NES core in original is PPC asm → they
  integrated FIX94's fixNES instead.
- Open issue #48: "Full 64-bit migration" (their direction; irrelevant to us
  — DC is ILP32).

## emu64 (the interception decision)

- Nintendo's own runtime N64→GC translation layer shipped in the game
  (`src/static/libforest/emu64/`, ~6k LOC, fully decompiled). Interprets
  F3DEX2-style display lists **extended with Dolphin-specific GBI commands**
  (SETTILE_DOLPHIN, SETCOMBINE_TEV/NOTEV, TRIN, custom ucode) and emits GX.
- Porter options: (a) keep emu64, implement GX (all existing ports do this);
  (b) intercept at N64-DL level with an sm64-style gfx backend — blocked by
  the non-stock GBI extensions; nobody has done it. **Decision: (a).**

## Other ports (techniques to borrow)

- **PS Vita — Brendonm17/ACGC-Vita-Port** (VitaGL + async compressed-tex
  branch, 960×544, render_scale option, 60 fps overworld after opt). 512 MB
  RAM — no low-RAM lessons for us.
- **3DS — AnimalCrossing-3ds-Port/ACGC-3ds** (created 2026-07-01, WIP,
  "terrible performance", wrong colors). New 3DS = 256 MB. Watch for their
  RAM strategy, but they haven't solved it either. DC (16 MB) is by far the
  smallest target attempted.
- **OpenCrossing-Anbernic** (our base) — see kb/base-repo-map.md.
- imcynic/ACGC-Deluxe-PC-Port — Deluxe ROM-hack content variant.
- No Switch/Wii/PSP/WASM ports exist (2026-08-01).

## N64 original (rejected alternative base)

- zeldaret/af (Dōbutsu no Mori decomp): WIP, **only partially shiftable**,
  README explicitly states porting is "a non-goal". No N64→PC port exists.
  The GC decomp is the only practical base despite its bigger memory
  footprint. https://github.com/zeldaret/af

## Legal model

- **No DMCA against any repo in this ecosystem** as of 2026-08-01 (decomp up
  since 2022; PC port since March 2026). Survival model: zero assets in
  repo, user supplies disc image, runtime/build-time extraction. Nintendo's
  recent GitHub DMCAs (2025-05, 2026-02) targeted Switch emulators +
  prod.keys circumvention, not decomps/ports.
- DC nuance: our output CDI **contains** assets → the CDI itself must never
  be distributed; repo ships tools only (dca3/sm64-dc model — both survived
  with builders + Colab notebooks).
- ac-decomp is CC0; flyngmt repo has no license (NOASSERTION). Keep our
  platform code clearly separated and credited.
