# Audio — verdict, plan of record, and what is still unmeasured

§9 and §7 of `kb/audio-plan.md`, moved verbatim: the recommended plan of record,
and the ranked list of measurements that would collapse the error bars.
Read before scheduling audio work or quoting an audio CPU number.
**Verdict: audio is a real risk, not a solved problem.**

---

## 🛑 0.0 THE STAGE-A-vs-STAGE-B DECISION IS REOPENED (2026-08-06)

**`src/` is no longer `-O0`.** The directive was reversed on 2026-08-06:
`-Os` everywhere in `src/` plus a 18-TU `-O3` hot list
(`DC_OPT_PROFILE=perf`, default), with `DC_OPT_PROFILE=o0` kept as a
byte-identical revert. `jaudio_NES` is `-Os` like the rest of the tree.

Every CPU input to §9's plan of record — and every input to §0's corrections
below — was modelled or measured on `-O0` code. In particular:

- **§0's "Step 2 is unchanged and now better supported… it is not a hedge any
  more" is WITHDRAWN.** That sentence rested entirely on the software mixer's
  cost being immovable. It is not immovable; it moved, by an unmeasured amount,
  in software's favour. **Stage B is a hedge again.**
- **§0's "Step 3 can no longer flip the decision" is WITHDRAWN** for the same
  reason. The live voice count is back to being decision-relevant.
- **The measured Flycast datum quoted below — audio on costs 45 % of the frame
  rate (FPS p50 23.5 → 13.0) — is an `-O0` measurement** and is stale as a cost
  for the current build. It is still a valid record of what that build did.

**Rule: do not scale these numbers, and do not re-affirm the verdict on them.**
No conversion factor is offered here because none is honest. The nearest
measured proxy the project has is the frameskipped logic tick — all of
`game_main` with the draw skipped, general `src/` code, which `jaudio_NES` also
is — which went **6.6 ms → 2.8 ms (−58 %)** across this change. A proxy is not
an audio result.

**What must happen before the A-vs-B call is taken again:** an audio-on town
smoke at `DC_OPT_PROFILE=perf`, against the same tree at `DC_OPT_PROFILE=o0`.
Both profiles exist and a full rebuild is 96 s, so this is a one-line A/B and it
should be measurement **#0** in §7 — ahead of the live voice count, because it
is cheaper and it gates the same decision. ⚠️ Read the `jammain_2.c` hazard
(`kb/audio-stage-a-software.md` §0) before the audio-on run: it is the file most
likely to miscompile under optimization in the whole tree, and it is on this
path.

Evidence, the full `-O0`/`-Os`/`-O3` table, and the tooling:
`kb/state-log.md`, entry **2026-08-06**.

---

## ⚠️ 0. The plan of record was written against a 2× arithmetic error (2026-08-05) — and §0 is itself now partly superseded by §0.0 above

**A jaudio DAC frame is 17.49 ms of audio, not "~35 ms".** `aictrl.c:292`'s
`AIInitDMA(..., DAC_SIZE * 2)` is 2,240 **bytes** — `DAC_SIZE` is in s16 units —
= 560 stereo pairs at `JAC_DAC_RATE = 32028.5` (`internal/rate.c:4-7`).
`kb/audio-engine.md` had 57.19 Hz right all along. Derivation and the second
correction (`rspsim.c` is `-O0`, so the 80-180 cyc/voice-sample band is
optimistic by ~1.7×): `kb/audio-cpu-cost.md` §0.

**What it does to §9 below, in order:**

- **Step 1's "expect 13 % CPU (9-20 %) at 10-12 voices" becomes ~22 %**, and
  the ≤12-18 % budget it was measured against is no longer met by any A0-A4
  config. The measured Flycast datum agrees: audio on costs **45 % of the frame
  rate** (FPS p50 23.5 → 13.0, `kb/STATE.md`), and synthesis alone runs at
  **0.88× real time ≈ 113 % of the SH-4**.
- ~~**Step 2 is unchanged and now better supported.** "Assume stage B is
  required" was hedged; it is not a hedge any more.~~
  🛑 **[WITHDRAWN 2026-08-06 — §0.0.]** It is a hedge again.
- ~~**Step 3 — the live voice count — is still worth taking**, but it can no
  longer flip the decision. Even 8-12 voices does not bring stage A inside the
  budget at `-O0`. It now sizes stage B's channel demand rather than deciding
  between A and B.~~
  🛑 **[WITHDRAWN 2026-08-06 — §0.0.]** Note the clause that carried it: "at
  `-O0`". Step 3 can flip the decision again.

⚠️ **One stage-B premise also moves**: `kb/audio-engine.md` §3.4's "no individual
soundfont exceeds 2 MB" is true of 2 MB and false of the **usable** sound RAM.
See that file's correction — bank 153 misses by 70,472 B.

---

## 9. Verdict and recommended plan of record

**Audio is a real risk.** It is not the fatal one — RAM and game-logic speed
still rank above it — but PLAN §3.4's framing ("Stage B *if* A blows the CPU
budget") should be inverted:

1. **Build stage A at M3** as specified in §4: 22.05 kHz, STEREO locked,
   FP round-trip removed, voice cap exposed as a setting, default 12.
   Expect **13% CPU (9–20%)** at 10–12 voices. It is the fastest path to
   correct sequenced audio and it is the byte-exact oracle for stage B.
2. **Assume stage B is required** ⚠️ **[the *assumption* is STALE 2026-08-06 —
   §0.0; the *action* is not]** and start the offline converter
   (`tools/bankconv`) at M1, in parallel — it is pure host-side work with no
   dependency on the DC toolchain, and §3's measurements say it will work:
   median sequence 49 KB in sound RAM (**measured 2026-08-13 at 50,628 B**),
   a single uniform codec to convert.
   🔴 **"no soundfont over 2 MB" IS FALSIFIED and is struck from this list.**
   It is true of 2 MB and false of the 1,900,544 B that is *usable*: bank 153
   is 1,971,002 B, over by 70,458 B. The ⚠️ at line 77 flagged it; this step
   was never rewritten. ⭐ **It is also no longer the blocker it looks like** —
   19 of bank 153's 126 samples exceed the 65,534-sample channel limit and are
   52 % of its bytes; without them it is 940,370 B. `kb/audio-aica-offload.md`
   §4.
   ⭐ **The converter now partly EXISTS: `tools/dcaudio/`** — a bit-exact
   VADPCM decoder, the `audiorom.img` reader, an AICA-ADPCM codec and the
   residency census. It does not yet pack a bank.
3. **Take measurement #1 in §7 (live voice count) on the PC build this week.**
   If AC really only runs 8–12 voices in a town, stage A at 22 kHz fits the
   budget and stage B becomes a quality/headroom upgrade rather than a
   requirement. If it runs 20+, stage B is mandatory and should be scheduled
   into M4 explicitly rather than listed as a contingency.

The things that make this tractable and were not known before this pass: the
bank is a **single uniform codec** (VADPCM order 2 / 2 predictors, 1157
samples), the residency unit is the **sequence** and its median size is
**49 KB**, and the two most expensive optional effects are **already disabled**
in the game's default sound mode.

---

## 7. What is NOT measured, and how to measure it

These are the numbers that would collapse the error bars. Ranked by value.

0. 🛑 **NEW AND FIRST, 2026-08-06 — what `-Os` did to the audio path.** Every
   entry below it was ranked on the assumption that `src/` codegen was frozen;
   it is not. Run an audio-on town smoke at `DC_OPT_PROFILE=perf`, then the same
   tree at `DC_OPT_PROFILE=o0`, and compare frame time and the audio ring's
   starvation rate. Both profiles exist and a full rebuild is 96 s, so this is
   the cheapest measurement in the list *and* it gates the biggest decision
   (stage A vs stage B). It also largely subsumes #3: it yields the real
   cyc/voice-sample of the shipping build without building `rspsim.c`
   standalone. ⚠️ `jammain_2.c` is the first suspect if the audio-on `perf` run
   misbehaves — see `kb/audio-stage-a-software.md` §0.
   Context: `kb/state-log.md` **2026-08-06**.

1. **Actual live voice count.** Instrument `Nas_DriveRsp` (`driver.c:541`):
   log `noteCount` and its max per second, on the existing PC build, across
   town / shop / museum / K.K. Saturday / a fishing tournament. This single
   number moves the CPU estimate by 2×. *Cheap — do it first, on the PC build,
   before any DC work.*
2. **Fraction of voices using `filter` / `comb_filter_size`.** Same harness,
   same run — count `common->filter != 0` and `comb_filter_size != 0` per
   frame. Decides whether cutting them is worth the perceptual loss.
3. **Real SH-4 cycles.** The 80–180 cyc/voice-sample band collapses to a
   measurement the moment `dc/build-dc-docker.sh` exists: build `rspsim.c`
   standalone for sh-elf, run a fixed 1000-frame command list under Flycast
   with the SH-4 cycle counter (`PMCR`/`TMU`), and report cycles per
   voice-sample. ~~**This is the single most valuable M1 measurement for
   audio.**~~ ⚠️ **[2026-08-06]** largely subsumed by #0, which gets the real
   number off the shipping build for the cost of two smoke runs — and note that
   this entry's whole point was to collapse a band that §0.2 of
   `kb/audio-cpu-cost.md` had widened for `-O0` reasons that no longer hold.
4. ✅ **PARTLY ANSWERED 2026-08-05. AICA sound-RAM reserve:** KOS's ARM driver
   region is **196,608 B** (`AICA_RAM_START 0x030000`,
   `aica_cmd_iface.h:37-38`), leaving **1,900,544 B** usable — so the ~200 KB
   estimate was right and the consequence is not: bank 153 needs 1,971,016 B
   and misses by **70,472 B**. Still unmeasured: what `snd_stream`'s own buffers
   take out of the remainder, which is the same question as whether stage B
   retires `snd_stream` entirely.
5. **AICA ADPCM loop-seam audibility** — convert the 451 looping samples,
   listen, and count how many need the 8-bit PCM fallback.
6. Whether the ~4.7% tempo error (57.19 Hz audio frames vs the engine's
   assumed 60 Hz `refresh_rate`) is inherited from GC or introduced by the PC
   port. It is not a new DC problem either way.
