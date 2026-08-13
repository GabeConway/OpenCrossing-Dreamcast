# Next session — the seed prompt, and the state it assumes

**Rewritten 2026-08-13**, at the end of the session that closed W1, finished the
villager investigation, and re-pointed everything at the AICA offload. This file
is the *paste-me* prompt plus the facts that live only in a human's head.

---

## 1. THE PROMPT TO PASTE (AICA offload)

> Read `CLAUDE.md`, then `kb/audio-aica-offload.md` in full, then
> `kb/audio-cheap-cpu-wins.md` §"W1 — CLOSED" and `kb/closed.md`'s W1 entry.
> We are implementing **stage B: move jaudio synthesis onto the AICA's hardware
> channels**. It is the single largest FPS lever left — `RspStart` is **14.4
> ms of a 76.4 ms frame, 18.9 % of busy CPU on real silicon**, the biggest
> symbol on the machine and 2.13× its Flycast share.
>
> **The host toolchain already exists and is tested: `tools/dcaudio/`.** A
> VADPCM decoder proven bit-exact against `rspsim.c` by a differential test
> (`tests/test_vadpcm.py`, 200/200), an `audiorom.img` reader, the Yamaha/AICA
> ADPCM codec, a residency census and a bank packer with `--verify`. Do not
> rebuild any of it. Run `python3 tools/dcaudio/census.py` to see the numbers.
>
> **Start at `kb/audio-aica-offload.md` §8** — the runtime design, which
> contradicts the obvious approach: voices must **NOT** be driven through KOS's
> `AICA_CMD_CHAN` queue (`snd_sh4_to_aica` has **no overflow check at all** and
> the ARM services at only ~430 Hz against a 229 Hz update tick). Drive
> volume/pan/pitch by **direct G2 register writes with SH-4-side shadows**, and
> use the queue only for key-on/key-off and voice setup.
>
> **And §11: the offload can be INCREMENTAL.** AICA sums all 64 hardware
> channels to the DAC and the software path already ends at two of them, so a
> voice on hardware and a voice from `RspStart` mix for free. Build it voice by
> voice with the software path live as the oracle (`DC_AUDIO_SOFTWARE=1`).
> ⚠️ The one real objection is **latency skew** — the software path is buffered
> ~100 ms while a direct key-on is immediate. §11 has two candidate fixes,
> neither tried.
>
> Do not re-propose the `CODEC_S8` bank (W1). It is **closed with arithmetic**:
> it needs +4,839,936 B of audio ARAM that the graph half cannot spare, and a
> mixed bank inside the real headroom converts 2.00 % of the bank. The AICA
> offload does not have that problem because its samples live in the
> Dreamcast's own 2 MB sound RAM, not the emulated GameCube ARAM.

---

## 2. What only exists in a human's head

- **FPS IS THE GATE ON EVERYTHING, and that is a human verdict, 2026-08-13:**
  *"i havent done it because the game runs so badly that it feels impossible to
  get that far."* The town has no villagers because nobody has finished Nook's
  opening job, and nobody has because the game is too slow. Do not treat FPS
  and content as competing priorities — content is downstream of FPS.
- **Audio sounds GOOD on hardware** and has since S14. Do not regress it. The
  offload's first working version drops reverb (`kb/audio-aica-offload.md` §12),
  which is an audible change, not a transparent one.
- **The SD adapter is the SCIF type**; `DC_GPROF_SD_IF` must stay 0 — probing
  for an absent card WEDGES with the console already muted.
- **The card must be MBR with FAT32 in PRIMARY entry 0.**
- **Holding L auto-advances dialogue** (`kb/traps.md`) — the difference between
  reaching Tom Nook and giving up.

---

## 3. Burns staged on the NAS, all built 2026-08-13, none burned yet

| file | what it is |
|---|---|
| **`AC-DC-20260813-FAST-silent.cdi`** | ⭐ **the one to try first.** Audio OFF (18.9 % of busy) + console muted (5.25 %) + villager knobs on. ~24 % of the frame back. **Silent by design** — it exists to make the opening completable |
| `AC-DC-20260813-pmcr-hud.cdi` | `cyc` / `istall` / `dstall` on the TV. **The only instrument that can say where the remaining frame goes**; blocked since 2026-08-08. Town, standing still, ~12 s after boot |
| `AC-DC-20260813-villagers-mute.cdi` | villager knobs on + mute, audio ON. Boot-verified (`margin=3364020 OK`) |
| `AC-DC-20260813-play-mute.cdi` | shipping config + mute, villager knobs off |
| `AC-DC-20260813-s8*.cdi` | 🔴 the W1 A/B pair. **Do not burn** — the S8 one is the broken bank |

⭐ **`DC_CONSOLE_MUTE=1` IS WORTH ~5.25 % AND EVERY BURN BEFORE TODAY PAID IT.**
`dc/build-dc.sh` now warns when `DC_CDI_PAD=1` is built without an explicit
choice. It is NOT a default: it silences crash dumps and would blind
`run_report.py` mid-smoke.

---

## 4. What this session settled, so it is not re-litigated

- ✅ **The villagers are NOT broken — it is a progression gate.**
  `kb/villagers-n3-result.md`. Six N3 runs; every wall is the game deliberately
  suppressing spawns during the intro. **Retired as live defects:** "nothing
  constructs a villager ACTOR", the save-path theory, the FGDATA/reserve-scan
  theory, N2b, and `mEv_CheckFirstIntro()` as culprit.
  ⚠️ Still unproven: no run has actually spawned a villager. The clean proof is
  a human finishing Nook's job on the FAST build.
- 🔴 **W1 (`CODEC_S8`) is CLOSED** — `kb/closed.md`. The codec was fine
  (38.56 dB median vs the VADPCM's 14-44); it does not fit ARAM.
- ⭐ **`MAC.W` on the ADPCM filter is PROVEN SAFE** over all 748,255 frames
  (`tools/dcaudio/bounds.py`) — the kb's warning was true of the FORMAT and
  false of THIS BANK. ⚠️ Margin is **one bit**: scale maxes at 12, 13 breaks it.
  ⚠️ Moot if the offload lands; its independent value is the resampler FIRs.

### ⚠️ The lesson this session paid for, in full

**Four counters were green while the S8 build was badly broken** —
`S8 bank OK`, `ASSET MISSING 0`, zero asserts, and `[NEOS_OUT]` peak 5807 vs a
baseline 5806, which was argued in writing as proof the conversion was correct.
It is not: peak amplitude is set by the loudest voices and **cannot see a
subset of instruments playing noise**. `aram_mapped` exceeded the 16 MB ARAM
address space *in the very table used to declare success*, and it was read past.
**A human listening for ten seconds was the only instrument that worked.**

---

## 5. The FPS picture, re-derived from the profile rather than quoted

`dc/build/gprof-runs/hw-title-1958f.flat.txt`, 1,889 frames, **busy = 76.4
ms/frame** — the machine is ~13 FPS CPU-bound at the *title screen*.

| | ms/frame | % busy |
|---|---:|---:|
| `RspStart` (audio) | 14.44 | **18.9** |
| `dc_gx_backend_submit` | 7.32 | **9.6** |
| `scif_write` + `scif_flush` | 4.33 | 5.7 ← muted now |
| `emu64_taskstart_r` | 2.94 | 3.9 |
| `set_position` | 2.82 | 3.7 |
| `cull_batch` | 2.67 | 3.5 |
| `memset` | 2.59 | 3.4 ← **never attributed** |

Top 10 symbols are 55 % of the frame. **There is no cheap win left**: the
audio offload (~19 %) and the indexed-submit rewrite (~9.6 %) are the only two
large ones, both multi-session.

⚠️ **`memset` at 3.4 % has never been attributed to a caller** and gprof here
is flat-only by design, so it needs instrumentation rather than inspection.

---

## 6. Recommended order

1. **The PMCR burn**, before either rewrite — it says where the other 58
   ms/frame goes and whether the draw path is icache-bound. Cheapest way to
   stop guessing.
2. **The AICA offload** (§1). Incremental, voice by voice.
3. The indexed-submit rewrite (G-B), `kb/research-sh4zam-gap.md`.
