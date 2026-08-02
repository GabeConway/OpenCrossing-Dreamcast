# Audio — verdict, plan of record, and what is still unmeasured

§9 and §7 of `kb/audio-plan.md`, moved verbatim: the recommended plan of record,
and the ranked list of measurements that would collapse the error bars.
Read before scheduling audio work or quoting an audio CPU number.
**Verdict: audio is a real risk, not a solved problem.**

---

## 9. Verdict and recommended plan of record

**Audio is a real risk.** It is not the fatal one — RAM and game-logic speed
still rank above it — but PLAN §3.4's framing ("Stage B *if* A blows the CPU
budget") should be inverted:

1. **Build stage A at M3** as specified in §4: 22.05 kHz, STEREO locked,
   FP round-trip removed, voice cap exposed as a setting, default 12.
   Expect **13% CPU (9–20%)** at 10–12 voices. It is the fastest path to
   correct sequenced audio and it is the byte-exact oracle for stage B.
2. **Assume stage B is required** and start the offline converter
   (`tools/bankconv`) at M1, in parallel — it is pure host-side work with no
   dependency on the DC toolchain, and §3's measurements say it will work:
   median sequence 49 KB in sound RAM, no soundfont over 2 MB, a single
   uniform codec to convert.
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
   voice-sample. **This is the single most valuable M1 measurement for audio.**
4. **AICA sound-RAM reserve** actually consumed by the KOS ARM driver +
   `snd_stream` buffers (the ~200 KB figure in §3.5 is an estimate).
5. **AICA ADPCM loop-seam audibility** — convert the 451 looping samples,
   listen, and count how many need the 8-bit PCM fallback.
6. Whether the ~4.7% tempo error (57.19 Hz audio frames vs the engine's
   assumed 60 Hz `refresh_rate`) is inherited from GC or introduced by the PC
   port. It is not a new DC problem either way.
