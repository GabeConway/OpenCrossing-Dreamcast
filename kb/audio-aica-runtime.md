# The AICA offload RUNTIME — built, wired, and RUNNING

**Written 2026-08-13.** `kb/audio-aica-offload.md` is the design and the host-side
measurements; **this file is what happened when it was implemented.**

**STATUS: the runtime is BUILT, LINKED, and RUNS. It has never been heard, and
it has no FPS number.** It is **default OFF** (`DC_AICA ?= 0`): both files
compile to stubs, the link carries no `--wrap`, and a plain build is
byte-identical to the pre-session tree — verified by symbol table (0 `wrap_Nas`
symbols, `dc_aica_init` = 4 bytes) and by a clean run.

🔴 **A "graphics fault" was reported against it and chased for most of a
session. It was an INCOMPLETE BUILD LINE — `DC_STUB_KEEP` was omitted, so every
image had ~53 KB of assets instead of 2,929,360 B. Nothing was broken. §7.**

⚠️ **CONSEQUENCE: every measurement in §5 was taken on that content-free image.**
The counters are real, the three bugs in §6 were real and are fixed, but the
offload has never run against the shipping workload and has never been heard.

Build with `kb/RESUME.md` §2's line **verbatim**, including `DC_STUB_KEEP`.

---

## 1. What exists

| file | what |
|---|---|
| `dc/include/dc_aica.h` | the API, and `DC_AICA_VOICES` (shared by both halves) |
| `dc/src/dc_aica.c` | HARDWARE half: sound-RAM arena, `aicabank.pak` index + residency, per-channel register driver, batched G2 flush, latency-matched key-on. No decomp header. |
| `dc/src/dc_aica_seam.c` | JAUDIO half: two `--wrap` interpositions, the divert policy, the parameter lift out of `AG.channels[]`. The only file including `jaudio_NES/`. |
| `dc/Makefile` | `DC_AICA` + 8 sub-knobs; `AICA_LDFLAGS` in `LINKSTATE` and on the link line |
| `dc/build-dc.sh` | forwards all 9 knobs |

Disc side: `python3 tools/dcaudio/pack.py --out <discroot>/aicabank.pak`
(4,749,792 B, `--verify` OK, 1157 entries / 24 excluded). Already staged in
`~/.cache/oc-dc-discroot`.

---

## 2. ⭐ THE SEAM, AND IT IS THE MAIN RESULT OF THE SESSION

`src/` is not editable, so the offload needs an interposition. There is exactly
one zero-cost gate and exactly one pair of cross-TU calls that reach it.

**The gate is `AG.common_channel[u*nch + i].enabled`.** `Nas_DriveRsp`
(`driver.c:541`) compacts the enabled snapshot rows into `noteIndices[]` and
walks only that list, so a voice not in it costs **nothing** — no decode, no
DMA, no resample, no `aEnvMixer2`, no `Acmd` bytes. ⚠️ Every other "mute" the
engine has (`sub.muted`, `group.flags.muted`, `pitch = 0`) still runs
`Nas_SynthMain` and still emits the envelope mixer. **They are attenuators, not
levers.**

`Nas_smzAudioFrame` runs **all four `__Nas_PushDrvReg` calls before any
`Nas_DriveRsp`**, and `Nas_MySeqMain` (`track.c:2121`) is called only from
`driver.c:180`. So one wrap on `Nas_MySeqMain` fires immediately before every
push, including the fourth. Second wrap on `Nas_smzAudioFrame`
(`driver.c:171`, called only from `sub_sys.c:743`) restores the flag at frame
exit.

**Why the restore is provably safe:** six other sites read
`AG.channels[i].common_ch.enabled` — `sub_sys.c:63`, `sub_sys.c:631`,
`memory.c:478/494/556/571`, `memory.c:911` — and **every one of them is outside
`Nas_smzAudioFrame`**, on the port-command drain and the spec-change/init paths.
`Nas_SpecChange` returning non-zero makes `CreateAudioTask` skip the frame
entirely. Verified 2026-08-13.

---

## 3. 🔴 TRAP: `--wrap` ON sh-elf, AND THE CLEVER SPELLING IS THE WRONG ONE

sh-elf prefixes user labels with `_`, so the obvious conclusion is
`--wrap=_Nas_MySeqMain` with an `__asm__("__wrap__Nas_MySeqMain")` label.
**That is wrong and it fails with a green build.** The wrappers are left
unreferenced, `--gc-sections` deletes them along with their `__real_*`
references, and not even an undefined symbol is reported.

⭐ **bfd strips the prefix itself.** `--wrap=X` matches `_X` and redirects to
`___wrap_X`, which is what an ordinary C function named `__wrap_X` emits.

```
link:  -Wl,--wrap=Nas_MySeqMain        (NO leading underscore)
code:  void __wrap_Nas_MySeqMain(...)  (plain C name, NO asm label)
```

Established by a four-way matrix in the SDK image, not by reading. The proof
the wrap bound is that `___wrap_Nas_MySeqMain` **survives `--gc-sections`**;
`wrapped=` on the console is the runtime check.

⚠️ `dc/src/dc_npctex.c`'s "WHY NOT `--wrap`" note and
`tools/dcstub/make_src_shrink.py:879` both assert the underscore theory. **They
are wrong.** This is the measurement.

---

## 4. ⭐⭐ THE ONE FINDING WORTH KEEPING: REVERB IS THE ENTIRE GATE

Title demo, 8,705 DAC frames, offload live:

```
rej_dsp split: fir=0  comb=0  haas=0  reverb=89178
```

**The 8-tap FIR, the comb filter and the Haas delay NEVER FIRE — not once.**
Every DSP rejection in the whole run is the reverb send, and it blocks **~58 %
of otherwise-eligible voice-updates.**

⭐ **This re-prices `kb/audio-aica-offload.md` §12 from "an open item nobody has
costed" to THE gate on stage B.** Three of the four effects the design cannot
reproduce are simply unused by this game; the fourth is on more than half the
voices. An AICA DSP send is not a polish item — it is most of the remaining win.
`DC_AICA_ALLOW_REVERB=1` diverts them anyway (dropping the send) so the cost can
be listened to rather than argued about.

Other rejections across the run: `synth=0 medium=0 codec=0 pack=0 novoice=0`.

---

## 5. What the run proved before the graphics fault

| claim | evidence |
|---|---|
| The interposition fires | `wrapped=34820` over 8,705 frames |
| ⭐ **The `device_addr` join key is EXACT** | `look=858 miss=0` — every live `smzwavetable.sample` resolved in the pack. §2 of the design doc is confirmed at runtime for the first time. |
| The bank loads off the disc | `loads=9 bytes=61272 fail=0`, arena 61,376 / 1,572,864 |
| Residency is not stressed | `resets=0 evicted=0` — the crude bump-allocator never filled |
| Voices persist and are driven | `updates=64578`, `flush=729 writes=1159`, `livepeak=5` |
| `FLAG_TOO_LONG` never bit | `toolong=0` |
| Latency correction arms | `delayed=858 now=0`, 124–165 ms — the ~100 ms §11 predicted |

---

## 6. Three bugs found and fixed by measurement (all in this session's code)

1. **A key-on per voice per update.** The frame-end restore alone left updates
   2/3/4 seeing update 1's cleared flag, so each tore the voice down and rebuilt
   it. `start nohv=39722 init=95` — 95 real notes, 39,722 rebuilds, ~40,000 full
   10-register reprograms. Fix: restore at the **top of every**
   `__wrap_Nas_MySeqMain` as well. Key-ons 39,817 → 739.
2. **Half the notes never sounded.** The one-shot lifetime was armed at *setup*,
   i.e. up to 130 ms before the delayed key-on, so every one-shot shorter than
   the skew correction was reaped while still pending. `keyon=448` against
   `keyoff=853`. Fix: `life_us` is a duration; `end_us` is set at key-on.
   → `keyon=855 / look=858`.
3. **`ai_dsp_sample_rate` is 0 while notes play.** The latency correction
   divided by it, got 0 µs, and silently took the immediate path every time
   (`lat rate=0 -> 0 us`, `delayed=0`) — the skew was uncorrected while the knob
   looked armed. Fix: latch the rate `snd_stream` was actually started at.

---

## 7. 🔴 THE "GRAPHICS FAULT" WAS AN INCOMPLETE BUILD LINE. NOTHING WAS BROKEN.

Reported from Flycast four times — offload on, `DC_AICA=0`, and finally with
`dc/` restored to HEAD and both new `.c` files moved out of the tree. Every one
looked wrong, which is what made it read as a pre-existing regression at HEAD.

**It was none of those. Every build in this session omitted `DC_STUB_KEEP`.**

`tools/dcstub/make_stub_data.py:1596` falls back to `DEFAULT_KEEP` when the
variable is unset, and `DEFAULT_KEEP` is **the title-logo overlay, ~53 KB, about
eleven files**. Everything else in the game — all 1,471 entries of
`keeplist-full.txt` — is stubbed to `u8 x[1]`. The correct line keeps
**3,261 table rows / 2,929,360 B**. Two orders of magnitude.

```
              kept assets       CDI
wrong line     ~53 KB           57,695,497 B     "graphics fucked up"
right line     2,929,360 B      61,145,769 B     "looks ok"
```

⚠️ **AND IT LOOKS EXACTLY LIKE A RENDERER BUG.** `ASSET MISSING 0`, zero
asserts, `dropped=0`, `clipped=0`, normal FPS — a stubbed array is not missing,
it is deliberately one byte, so every counter is honest and green while the
geometry is gone. `kb/traps.md` already carried this ("a stubbed asset looks
like a renderer bug"); it was not applied because the *build line*, not the keep
list, was the thing left out.

⭐ **THE LESSON IS THE ONE `kb/RESUME.md` §2 ALREADY STATES**: *"a result that
lives only in a command line is one unset environment variable away from being
lost."* Use the documented shipping line verbatim. It is not a list of
optional tuning knobs — `DC_STUB_KEEP` decides whether the game has any content
at all, and `DC_ARAM_WINDOW` / `DC_ARENA_BYTES` were omitted here too.

⚠️ **A SECOND SELF-INFLICTED SYMPTOM, same session**: with
`DC_FB_PROBE=150 DC_FB_IMAGE=2` the run hitches hard — one frame measured
`total=1016.9ms` against `snd=23.6ms gx=5.8ms swap=0.2ms`, i.e. ~987 ms
unaccounted. That is the probe: 12,480 `FBROW` lines is ~5.5 MB of base64 over
SCIF, and a screenshot emits 320 of them inside a single frame. **Never judge
smoothness on a `DC_FB_IMAGE` build.**

⭐ **CONSEQUENCE FOR STAGE B: it has never actually been evaluated.** Every
`DC_AICA=1` run in §5 was made on the content-free image, so the counters there
are real but the audio workload behind them is not the shipping one. The three
bugs in §6 were genuine and are genuinely fixed; the offload's *behaviour* is
still unheard and unmeasured. Re-run it on the correct build line before
believing anything about it.

## 8. What is still not built

- **No reverb send** — and §4 says that is now the main item, not a footnote.
- **No residency policy.** The arena is a bump allocator that resets wholesale
  (`resets=`); the per-sequence LRU of `kb/audio-aica-offload.md` §9 is unbuilt,
  and `resets=0` says nothing has yet needed it.
- **No FPS number.** The A/B (`DC_AICA_DIVERT=1` vs `=0`) was built but never
  run, because the graphics fault made any timing meaningless.
  ⚠️ And when it is run: Flycast understates audio **2.13×** (measurement rule
  12), so an emulator delta is a floor.
- **The §6 loop-state burn** (does the AICA carry ADPCM decoder state across the
  loop point?) is untouched. `DC_AICA_SM_LOOP=2` is wired for it.
- **`late=209` of 858 key-ons** miss their deadline by >20 ms — the pump runs
  once per logic tick, which is coarser than the skew correction wants.
  §11 fix (2) is the alternative, and its jiffy clock is the wedged one.
