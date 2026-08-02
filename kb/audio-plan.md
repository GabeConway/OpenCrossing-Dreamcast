# Audio plan — jaudio_NES on Dreamcast

Written 2026-08-01 (measure pass). Supersedes the sketch in `PLAN.md` §3.4.

**Verdict up front: audio is a real risk, not a solved problem.** Software
synthesis (stage A) is *buildable* and is the right bring-up path, but at the
game's shipped settings it costs an estimated **~68% of the SH-4** and even at
22 kHz with effects trimmed it costs **~34%** at the full 24-voice cap — which
does not fit alongside PLAN's ≤25 ms game-logic gate at 30 fps. Plan on
stage B (AICA hardware voices) being **required**, not a fallback. The good
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
