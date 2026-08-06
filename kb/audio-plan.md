# Audio plan — jaudio_NES on Dreamcast

Written 2026-08-01 (measure pass). Supersedes the sketch in `PLAN.md` §3.4.

---

## ⚠️ THE VERDICT BELOW IS REOPENED — ITS PREMISE DIED ON 2026-08-06

**Every CPU number in this document and in all five parts was modelled or
measured against `src/` compiled at `-O0`, and `src/` is not `-O0` any more.**
The ban was reversed on 2026-08-06: `src/` builds at `-Os` with a 14-TU `-O3`
hot list (`DC_OPT_PROFILE=perf`, the default), and `jaudio_NES` — `rspsim.c`,
`driver.c`, all of it — is `-Os` along with the rest of the tree.

So the sentence this whole plan turns on — *"the SH-4 cost of software
synthesis is a fixed input, therefore stage B is required"* — **is VOID.** The
cost of the software mixer is now an **open question**, not a constant.
**The stage-A-vs-stage-B decision must not be taken again until it is
re-measured.**

**Do not scale the old numbers.** This document may not invent a conversion
factor, and neither may you. The nearest measured proxy the project has for
what optimization did to general `src/` code is the frameskipped logic tick —
all of `game_main` with the draw skipped — which went **6.6 ms → 2.8 ms, a
58 % fall**; `jaudio_NES` is general `src/` code, but a proxy is not a result.

**The re-measurement is one line now.** Run an audio-on town smoke at
`DC_OPT_PROFILE=perf`, then the same tree at `DC_OPT_PROFILE=o0` (the
byte-identical revert), and diff. Both profiles exist and a full rebuild is
96 s.

⚠️ **Before that run, read the `jammain_2.c` hazard** in
`kb/audio-stage-a-software.md` §0 — the warnscan named it the single riskiest
file in the tree, and it is on the audio path.

Evidence for all of the above: `kb/state-log.md`, the **2026-08-06** entry.

---

**Verdict up front [STALE 2026-08-06 — see the block above]: audio is a real
risk, not a solved problem.** Software
synthesis (stage A) is *buildable* and is the right bring-up path, but at the
game's shipped settings it costs an estimated **~68% of the SH-4** and even at
22 kHz with effects trimmed it costs **~34%** at the full 24-voice cap — which
does not fit alongside PLAN's ≤25 ms game-logic gate at 30 fps. ~~Plan on
stage B (AICA hardware voices) being **required**, not a fallback.~~
[STALE 2026-08-06 — "required" rested on the `-O0` cost being immovable; it is
not, and stage B is back to being *unranked* against stage A until the A/B
above is run.] The good
news, all newly measured: the bank format converts cleanly, the *per-sequence*
sample working set is tiny (median 49 KB, 247 of 249 sequences use exactly one
soundfont), and the two most expensive per-voice effects are already off in the
game's default sound mode.

Everything below marked **[measured]** was derived by parsing the real
`audiorom.img` out of the user's GAFE01 ISO and the real engine source.
Everything marked **[modelled]** is an arithmetic estimate with stated error
bars — no SH-4 hardware or toolchain existed when this was written.

---

## This document was split (2026-08-02)

It was 599 lines. The content below is unchanged and lives in five parts; the
verdict above stays here because it governs all of them. Section numbers inside
the parts are the original ones — use this table to resolve a `§n` reference.

| part | original sections | contents |
|---|---|---|
| [`kb/audio-engine.md`](audio-engine.md) | §1, §3 | what the engine is (N64 libaudio, not JAudio), shipped parameters, `audiorom.img` layout/encoding/sizes, AICA constraints |
| [`kb/audio-cpu-cost.md`](audio-cpu-cost.md) | §2, §8 | per-voice op counts, SH-4 cycle model, A0–A4 configs, sm64-dc precedent |
| [`kb/audio-stage-a-software.md`](audio-stage-a-software.md) | §4 | stage A: rspsim at 22 kHz, effect-cut order, `snd_stream` plumbing, footprint |
| [`kb/audio-stage-b-aica.md`](audio-stage-b-aica.md) | §5, §6 | stage B: the seam, register map, residency policy, the offline `tools/` |
| [`kb/audio-plan-of-record.md`](audio-plan-of-record.md) | §9, §7 | the plan of record, and the ranked list of what is still unmeasured |
