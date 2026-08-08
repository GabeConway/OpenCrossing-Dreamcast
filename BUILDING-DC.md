# Building OpenCrossing for the Sega Dreamcast

Everything happens inside one Docker image, `opencrossing-dc:sdk`, which
carries sh-elf-gcc 15.2.0, KallistiOS, GLdc (`-lGL`), zlib and `mkdcdisc`.
The host needs nothing but Docker (colima on this machine).

> `pc/` is the Linux/SDL reference port. It is **not** the Dreamcast build.
> Do not run CMake for Dreamcast — the DC build is a plain GNU makefile.

---

## Quick start

```bash
cd /Users/gabe/Documents/GitHub/OpenCrossing-Dreamcast

bash dc/build-dc-image.sh        # once, ~27 min. Skip if the image exists.
bash dc/build-dc.sh              # ELF + CDI
```

Artifacts land in `dc/build/` (gitignored — **never** commit a disc image):

| File | What it is |
|---|---|
| `dc/build/AnimalCrossing.elf` | unstripped ELF. Keep it: `sh-elf-addr2line -e` on it turns a crash PC into file:line. |
| `dc/build/AnimalCrossing.map` | link map |
| `dc/build/OpenCrossing.cdi` | the disc image |
| `dc/build/obj/**` | objects + `.d` files, mirroring the source tree |

---

## The three entry points

| File | Runs where | Job |
|---|---|---|
| `dc/build-dc.sh` | host | `docker run` wrapper. Checks the image exists, forwards env, mounts the repo at `/work`. |
| `dc/build-dc-docker.sh` | container | drives `make`, then `mkdcdisc`. |
| `dc/Makefile` | container | the actual build. |

You can also drive `make` directly:

```bash
docker run --rm --platform linux/arm64 \
  -v /Users/gabe/Documents/GitHub/OpenCrossing-Dreamcast:/work \
  opencrossing-dc:sdk bash -c 'make -C /work/dc -j4 objs'
```

`--platform linux/arm64` is not optional. Without it an accidental amd64 pull
drops the whole build into qemu — slow and flaky
(`kb/design-toolchain.md` §2).

---

## Make targets

| Target | Effect |
|---|---|
| `make objs` | compile every TU, **do not link**. This is the milestone-1 signal. |
| `make all` (default) | `objs` + link → `dc/build/AnimalCrossing.elf` |
| `make clean` | `rm -rf dc/build` |
| `make count` | print TU counts |
| `make sources` | dump the computed source list — use this when debugging the exclusion filters |

`make count` should print **3917 TUs**:

| | count |
|---|---|
| decomp `.c` (after the 35 inherited PC filters) | 3854 |
| decomp `.cpp` | 46 |
| `src/static/dolphin/pad/Padclamp.c` (added back) | 1 |
| `pc/src` files reused verbatim | 3 |
| `dc/src/*.c` | 13 |

3854 + 46 = 3900 is exactly the number `kb/design-shelf-hazards.md` §0
measured as compiling for sh-elf, so a different number means a filter broke.

Parallel and incremental builds both work: `make -j8`, and re-running `make`
after touching one file rebuilds only what depends on it (GCC `-MMD -MP`
dependency files live next to the objects). Measured on this host, inside
colima (4 cores), `-j4`:

| | time |
|---|---|
| clean `make all` (3917 TUs + link + CDI) | **97 s** |
| `touch include/m_play.h` → `make objs` (2604 TUs) | 78 s |
| no-op `make objs` | < 2 s |

---

## Environment knobs

| Var | Default | Meaning |
|---|---|---|
| `JOBS` | `4` | `make -j` level. The colima VM has 4 cores. |
| `DC_TARGET` | `all` | pass `objs` for a compile-only run |
| `DC_CDI_PAD` | `0` | `1` → padded 740 MB CDI (see below) |
| `DC_ASSET_STUB` | `0` | `1` → the throwaway bring-up image (see below) |
| `DC_BGTEX_DEMAND` | `1` | **R1** — the 27 acre ground textures are read off `/cd/foresta.rel` on demand (`dc/src/dc_bgtex.c`) instead of living in `.bss`. Worth 80,736 B of keep list, and it is what makes the WINTER town's ground render at all: the keep list could only ever afford one season, so `mFM_grd_w_*` was stubbed and December's ground was black. `0` restores the vendored `bcopy` **and** puts the 27 `mFM_grd_*.c` files back on the keep list. One value drives both `tools/dcstub` generators and the `-D`; the generated `m_field_make.c` `#error`s if they disagree |
| `DC_NPCTEX_POOL` | `1` | **R2** — the 236 villager texture sets (993,984 B) are served out of 16 static slots read off `/cd/foresta.rel` (`dc/src/dc_npctex.c`) instead of living in `.bss`. A town holds at most `ANIMAL_NUM_MAX`=15 villagers + 1 islander, so 16 slots is the provable ceiling; the keep list was paying 90,464 B for the 21 sets a census saw and leaving the other 215 species untextured. ⚠️ The SPECIAL NPCs (rows ≥ `ALL_NPC_NUM` — Tom Nook, Rover, K.K., Porter, the raccoons) are **not** pooled and stay on the keep list. `0` restores those 21 keep-list entries and inserts no calls. One value drives both `tools/dcstub` generators and the `-D`; all three generated TUs `#error` if they disagree |
| `DC_NPCTEX_SLOTS` | `16` | R2's slot count, 4,832 B of `.bss` each. Raise it only against a `[DC/NPCTEX] POOL FULL` line — that is an event/mask NPC borrowing a villager row, which the 15+1 ceiling does not count |
| `DC_DISC_ROOT` | unset | a directory whose files go on the disc **flat** |
| `DC_OPT_PROFILE` | `perf` | `perf` = `-Os` + `-O3` on `dc/opt-lists.mk`'s hot list · `size` = `-Os` everywhere · **`o0` = byte-identical revert to the pre-2026-08-06 build**. See the optimization section below |
| `DECOMP_OPT` | from profile | optimization level for decomp game code; overrides the profile |
| `DECOMP_HOT_OPT` | from profile | optimization level for the hot list; overrides the profile |
| `DC_OPT_O0_EXTRA` | unset | extra sources forced to `-O0` without editing `dc/opt-lists.mk`. The bisecting knob (`tools/dcopt/bisect_o0.sh`) |
| `DC_AUTOVAR_INIT` | unset | `zero` → `-ftrivial-auto-var-init=zero`. The A/B for the 99 uninitialised reads the warnscan found |
| `DC_OPT` | `-O2` | optimization level for `dc/src` platform code |
| `DC_ARENA_BYTES` | header | arena size (bucket 6). **Shrink, never grow** — it competes with libc |
| `DC_ARAM_WINDOW` | header | resident graph-ARAM window. Floor 851,968 (`forest_1st.arc`) |
| `DC_DIAG` | `0` | `1` → `PC_DIAG()` bring-up tracing inside `graph_proc` |
| `DC_FB_PROBE` | unset | `<N>` → guest-side screenshot every N presented frames. Needs `smoke.sh --fb-writeback` to see anything |
| `DC_ARENA_PROBE` | unset | `<N>` → arena touched/used + libc break every N presented frames |
| `DC_EMU64_HIST` | `0` | `<N>` → **G1, the per-opcode emu64 time histogram**, sampled one presented frame in N. Prints `[EMU64H] f= tot= gap= probe= \| NAME ms/calls …` next to `[EMU64]`. This is the only instrument that can say WHICH of the town's 2,094 state commands per frame cost the ~26 ms they cost. It installs timing thunks into emu64's own dispatch table at runtime and swaps the originals back, so non-sampled frames are bit-identical to a clean build; `src/` is neither edited nor recompiled, and the only build change is one `objcopy --globalize-symbol` promoting an existing local symbol. **Read `tot=` first** — it must land near `[PHASE] draw=` minus `gx=` for the same window, and if it does not, the histogram is wrong. `probe=` is the measured per-command instrument cost, published so a bucket can be corrected rather than argued about |
| `DC_ASSET_CENSUS` | unset | `1` → record the asset addresses the GX layer is handed; resolve with `tools/dcstub/census_resolve.py` |
| `DC_STUB_KEEP` | logo list | `:`-separated sources the stubber leaves FULL SIZE. Generate it from a census with `tools/dcstub/census_keeplist.py` |
| `DC_AUTOSTART` | unset | `<N>` → synthesise START/A from `PADRead` call N onward. **The only way an unattended run gets past the title screen** |
| `DC_AUTOSTART_PERIOD` | `90` | calls between synthesised pulses (each pulse is 6 calls) |
| `DC_AUTOSTART_START_EVERY` | `4` | every Nth pulse is START, the rest are A. **A-dominant on purpose** — past the title, dialogue takes A or B only (`m_msg_normal.c_inc:2`) and choice menus default to index 0, so a 1:1 alternation wasted half of every run. START is needed at exactly one place on the path to the town: ending the name-entry keyboard (`m_editor_ovl.c:447`). `=2` restores the old 1:1 pattern |
| `DC_AUTOWALK` | unset | `<N>` → synthesise ANALOG STICK movement from `PADRead` call N onward: a deterministic 8-direction compass walk. `DC_AUTOSTART` only presses buttons, so before this an unattended run reached the town and then **stood still for 600 s**. That hole has already cost a bug — the station roof clip-through (`kb/station-bugs.md` §2) is human-reported twice and has never appeared in a captured frame, because nothing could walk a character under the roof. It also widens the screenshot gate from one composition to a tour of the town. A real stick always overrides it |
| `DC_AUTOWALK_SEG` | `240` | `PADRead` calls per leg before turning 45°. ~8-20 s per leg at 12-30 FPS |
| `DC_CONSOLE_LIMIT` | `1` | `0` → kill switch for the `printf`/`OSReport` flood limiter |
| `DC_FB_IMAGE` | unset | `<1\|2\|4>` → with `DC_FB_PROBE`, stream the whole frame out as base64 `FBROW` lines, box-filtered by that factor. Decode with `tools/dcfb/fbimg_to_png.py` |
| `DC_TEX_LOG` | unset | `1` → one line per texture **upload** describing what the decoder produced. Separates "never uploaded" from "uploaded as a blank rectangle", which the uploads/hits/evictions counters cannot |
| `DC_PVR_BATCH_LOG` | unset | `<N>` → dump every renderer batch's state on every Nth frame. Set it to the **same** N as `DC_FB_PROBE` so the lines describe the captured frame. Fields: `ac=` alpha compare asked for, `cut=` treated as a cutout, `cu=` colour/alpha update, `tm=` stage 0/1 texmap (`255` = `GX_TEXMAP_NULL`), `st=` TEV stage count, `t1=` whether texmap1 binds a *different* image, plus screen bbox, z and uv ranges |
| `DC_ARAM_TBL_PROBE` | unset (0) | `1` → log every 64-byte ARAM read. That size is uniquely `mMsg_Get_BodyParam`'s resource-TABLE fetch (`m_msg_main.c_inc:284,289`), and MESSAGE (works) and STRING/SELECT (broken) both use it, so one run gives the control and the failure side by side. Decision table is in the comment at the probe |
| `DC_ARAM_AUDIO_DROP` | `1` | **⚠️ SET IT TO `0` WHENEVER AUDIO IS ON — at the default there is NO SOUND.** All 8,300,384 B of `audiorom.img` is discarded at `dc_aram.c:317-320` *before* `dc_dvd_provenance()` at `:325`, the audio half of ARAM gets zero extents, and every jaudio sample fetch is `memset` to zero: a live pipe carrying silence, with `[NEOS_OUT] ... peak=0` as the only tell. **PROVEN GOOD 2026-08-06** (it was "unproven and risky" here until then, and the risk did not materialise): `mapped` 4,982,400 → **13,282,784** (exactly +8,300,384), `ext=3/32`, `LOST=0`, `drop=0`, `zero=0`, `stored=0`, `pin_peak=0`, `peak` 0 → 3851, human-verified on real hardware. Costs no pool bytes — `dc_aram.c:344` skips block allocation for mapped writes |
| `DC_AUDIO` | `0` | `1` → open the AICA output device and run the jaudio synthesis pump. **Audio works and costs ~45 % of the frame rate** (FPS p50 23.5 off / 13.0 on, matched 600 s runs), which is why it is off. See **Audio: the scene gate** below before setting it |
| `DC_AUDIO_SCENES` | unset (= `all` when `DC_AUDIO=1`) | which `sou_scene_mode` values run synthesis, e.g. `3`, `0,3,18`, `all`, `none`. **Setting it implies `DC_AUDIO=1`.** This is the knob you want — see below |
| `DC_AUDIO_BUDGET_US` | `0` (off) | ⚠️ **opt-in, and measured WORSE.** The per-frame predictive ceiling on synthesis. The three matched runs are 23.5 (off) / 13.0 (audio, no budget) / **10.9 (audio + budget, and it reached a shallower scene: 4 vs 18)**. It loses because the loop must override the budget when the ring is starving, and at ~19.8 ms/DAC frame the ring starves essentially always, so the override becomes the normal path. Kept only to reproduce that run. Reach it with `DC_XDEFS` |
| `DC_AUDIO_MAX_FRAMES` | `2` | hard cap on jaudio DAC frames per pump — an integer bound with no measurement and no override, so unlike the budget it cannot feed back on itself. It binds only on the first pump after the scene gate arms, where an empty ring would otherwise ask for three frames (~59 ms) in one call. `DC_XDEFS` |
| `DC_AUDIO_SCENE_SETTLE` | `2` | consecutive pumps a new gate answer must survive before it is acted on. `0` disables the debounce. `DC_XDEFS` |
| `DC_AUDIO_FADE_STEP` | `1` | output-gain ramp in Q8 steps per stereo frame on each gate transition; `1` ≈ 8 ms at 32 kHz. Raise to fade faster, at the cost of an audible edge. `DC_XDEFS` |
| `DC_AUDIO_HEADROOM` | `2048` | samples kept free at the top of the ring. ⚠️ Do **not** gate the pump on "ring less than half full" — a stalled consumer then leaves it half full forever and synthesis never runs (measured deadlock, `synth_frames=0`) |
| `DC_AUDIO_VOICES` | `0` (= shipped 24) | **L1 — the peak-cost lever.** Overwrites `NA_SPEC_CONFIG[0]._05` (`audioconst.c:10`) in `AIInit`, before `Nas_SpecChange` reads it at `memory.c:1001`, so **zero `src/` edits**. Per-voice work is exactly linear — `updates_per_frame(4) × num_samples_per_update(200) = 800` internal samples per enabled voice — so this halves the WORST DAC frame while barely moving the mean. Needs no drop logic: allocation already steals lowest-priority-first through `__Nas_GetLowerPrio` (`channel.c:788-811`) and refuses to steal from an equal-or-higher priority note, so a short pool degrades to priority-ordered note-stealing. Also fixes a latent overrun — `AG.max_audio_cmds` is 2,350 at 24 voices but the DC path writes into `pc_task_buf[2][1600]` (`neosthread.c:26`) and never checks the bound. `DC_XDEFS` |
| `DC_AUDIO_MIXRATE` | `0` (= shipped 48000) | **L2 — the other peak-cost lever**, same seam. This is the INTERNAL mix rate (`NA_SPEC_CONFIG[0]._00`), not the output rate: halving it halves `num_samples_per_update` (`memory.c:974`) and so halves per-voice work in the mean AND the peak. ⚠️ **Pitch is preserved** — the resample error term is `33476.156 / JAC_DAC_RATE` (`oneshot.c:900` × `memory.c:993`), which does not contain this field. Output rate, `DAC_SIZE`, `JAC_FRAMESAMPLES` and the `snd_stream` open are untouched; `Jac_Resample16` (`rspsim.c:682`) derives its step at runtime and simply upsamples instead of downsampling. Riskier than `DC_AUDIO_VOICES` — it trades the high band for CPU. ⚠️ Pick a rate where `rate/60/4` is a multiple of 8. `DC_XDEFS` |
| `DC_AUDIO_VOICELOG` | unset | **L0 — the instrument that prices L1/L2, and is allowed to kill them.** Walks `AG.common_channel[]` after each `pc_audio_process_frame()` and counts the same `enabled` predicate `Nas_DriveRsp` tests (`driver.c:555`), i.e. the literal `noteCount`. Adds `vmax= v@= filt@= comb@=` to `[STUTTER]` (`v@`/`filt@`/`comb@` describe the SAME call that set `smax=`) and a whole-run `[DC/VOICE] v>=N n= mean= max=` histogram bucketing synthesis cost BY voice count. **If mean and max are flat across buckets, the voice-count hypothesis is dead in one run** and the search moves to per-voice conditional work — which is what `filt@`/`comb@` are for (FIR `driver.c:1183-1188`, +27 % on an affected voice; comb `:1190-1209`, +10 %; incidence never measured). Costs 96 loads per DAC frame, unmeasurable against a 2,500-10,000 µs frame. ⚠️ Includes a decomp header, per the `DC_AUDIO_HEAPLOG` precedent. `DC_XDEFS` |
| `DC_KEEP_SWEEP` | `1` | `0` → load the `DC_STUB_KEEP` assets in source order, one `fs_seek`+`fs_read` each (the pre-2026-08-03 behaviour). On `1` the requests are recorded on one pass, sorted by `(rom_src, rom_off)` and replayed through a sector-aligned window: **578 → 130 disc reads** for 1,392 assets. Hardware-only win — Flycast's `FastGDRomLoad` hides it, so A/B the `reads=` count in the `[DC/KEEP] sweep:` line, not the clock |
| `DC_KEEP_SWEEP_WIN` | `16384` | sweep window bytes. Modelled: 8 KB → 193 reads, 16 KB → 129, 32 KB → 89, for +1.6/+2.1/+2.9 MB read. 16 KB is the knee. Transient RAM, freed before the arena is carved |
| `DC_KEEP_TRACE` | unset | `1` → restore the per-asset `[DC/KEEP] <name> <n> B @ <off>` line. ⚠️ **Costs 15.0 s of boot on hardware** — 1,392 lines = 86,357 B, and KOS busy-waits on the SCIF TX FIFO at 57,600 baud whether or not a cable is attached. Only useful when auditing what a new keep list actually pulled |
| `DC_NO_LATE_PVR` | unset | put `pvr_init()` back inside `dc_gx_init()`. By default the PVR is brought up by `dc_gx_backend_start()` **after** the asset load, so the boot splash stays on screen for the whole load instead of the screen going black the moment `dc_gx_init()` runs |
| `DC_NO_SPLASH` | unset | remove the "TechProGabe Presents..." boot splash. `DC_SPLASH_MS` (default `2000`) is its **minimum** on-screen time — it is not cleared, so it persists until the PVR takes the display |
| `DC_SPLASH_NO_THANKS` | unset | drop the "SPECIAL THANKS TO" block under the splash's progress bar (ACreTeam, Cuyler36, Dia2809, flyngmt, Falco Girgis — the README's Hall of Heroes, same order). Costs no RAM: bfont is in the BIOS and the three lines are 94 B of `.rodata`. Drawn once, below the animated band and the bar, so the load animation never repaints over it |
| `DC_PAD_NO_LATCH` | unset | stop accumulating button presses at 60 Hz. By default a vblank handler ORs every button it sees into a sticky mask that `PADRead` consumes, so a tap that goes down and up between two logic ticks still produces an edge. Matters at hardware frame rates: at ~11 FPS the sample period is 91 ms and a tap can otherwise be lost entirely |
| `DC_SCIF_FAST` | unset | `1` → raise the console from KOS's 57,600 baud to **1,562,500** (~5.8 KB/s → ~150 KB/s), the rate the harness selftest has used since M0. ⚠️ **Emulator only** — a real coder's cable will not sync at 1.5 Mbps and a hardware build with this set has no console and no crash dump. What it buys: a `DC_FB_IMAGE` screenshot drops from ~35 s to ~1.4 s, so a screenshot run stops being a different experiment from a progression run |
| `DC_XDEFS` | unset | raw extra `-D` flags, appended last. How the renderer kill switches are reached — see below |
| `V` | unset | `V=1` echoes full compiler command lines |

### Audio: the scene gate

**The cost of sound is not the same in every scene, but until now the switch
was.** One jaudio DAC frame is ~19.8 ms of SH-4 for ~35 ms of audio, so
synthesis needs ~57 % of the machine to stay level. Measured 2026-08-04:

| scene | `sou_scene_mode` | vertices/frame | FPS, audio off |
|---|---|---|---|
| K.K. / player-select | 3 | 888 | 29.9 — **at the frame cap**, `gx=4.1 ms` |
| outdoor town | 9 | ~6,951 | 14.9, and never higher, `gx≈13 ms` |

The town cannot afford audio. The K.K. scene has headroom, and it is the scene
where the silence is most obviously wrong: `ac_npc_p_sel_schedule.c_inc:71`
opens it with `p_sel->strum_timer = 440` — **7.3 s of K.K. strumming a guitar**
with `[TRG_VOL] slot=0 id=0x044D vol=1.000` firing every tick, rendered silent.
It reads as "silent loading". It is not loading.

```bash
# sound in the K.K. scene only, silent everywhere else
DC_AUDIO_SCENES=3 DC_XDEFS='-DDC_ARAM_AUDIO_DROP=0' bash dc/build-dc.sh
```

Modes: `0` title (**also the `.bss` zero value — "no scene chosen yet"**, so
listing 0 arms audio from the first pump), `3` K.K./player-select, `4` train
intro, `9` outdoor town, `18` name entry. Separators may be `,`, `:` or space.
`all`/`*` is everywhere (what `DC_AUDIO=1` has always meant); `none` or an empty
list means the SPU device is never even opened, which is the runtime equivalent
of `DC_AUDIO=0`.

**⚠️ Also pass `-DDC_ARAM_AUDIO_DROP=0`.** Otherwise the ARAM pager throws
`audiorom.img` away and jaudio synthesises from zeroed samples — the run is
silent, at full CPU cost, and every audio counter still looks healthy.

**What it does and does not do.** The gate skips **synthesis**; it never stops
the stream. `snd_stream` keeps one AICA channel keyed on with a looping buffer,
so "stop feeding it" is not silence — it is the last ~128 ms looping forever.
The pump therefore keeps polling in a disarmed scene, the callback zero-fills an
empty ring, and the channel plays digital silence at a cost of two G2 accesses
and an 8 KB `memset`. Stopping and restarting the stream per scene change was
rejected because both halves travel the SH-4→ARM command queue, and that queue
is only serviced because `dc_aica_clock_kick()` is hand-driving `AICA_MEM_CLOCK`
past a Timer-A FIQ that never fires; a dropped start would leave the console
silent for the rest of the run with no path back.

**Why this is not the `DC_AUDIO_BUDGET_US` mistake repeated.** The budget
decided from ring fill, its decision changed ring fill, and its anti-gap
override closed the loop — it scored *worse* than no budget at all. The gate's
input is `sou_scene_mode`, which nothing in the audio path can influence and
which the game writes only at scene construction. The answer is therefore
constant for a whole scene, cannot oscillate, and has **no override**: a
disarmed scene stays disarmed even while the ring starves, because a starving
ring in a disarmed scene is the intended state.

**Reading a run.** `[DC/AUDIO] gate scene=… armed=… arms=… disarms=… gain=…`
every 600 pumps, plus one line per transition. `arms=0` with a `[SCENE_MODE]`
edge into a listed scene means the gate never fired; check the mask on the
`[DC/AUDIO] scene gate:` line printed at `AIInit`.

**Unmeasured.** The K.K.-scene FPS *with* audio armed has not been run. The
scene is at the 30 FPS cap today, and one DAC frame per game frame is ~19.8 ms
of a ~33 ms budget, so expect it to drop off the cap — somewhere in the 20s, not
the 14.9 the town pays. A/B it with `run_report.py --vs` and read `arms=`,
`fps_p50` and `deepest_scene` together.

### Renderer kill switches (`DC_XDEFS`)

Compile-time A/B knobs in `dc/src/dc_pvr.c`. Each isolates one convention that
has already been got wrong at least once, so a single build settles the
question instead of an argument from source.

| define | effect |
|---|---|
| `DC_PVR_NO_UVCLAMP` | ignore the GX wrap mode; every texture repeats (the pre-2026-08-02 behaviour) |
| `DC_PVR_NO_TEVCONST` | ignore TEV constant colours; stage 0's colour is always the rasterised colour |
| `DC_PVR_NO_CULL` | `PVR_CULLING_NONE` everywhere |
| `DC_PVR_CULL_INVERT` | swap CW/CCW — restores the old, wrong cull mapping |
| `DC_PVR_NO_LIGHTING` | skip GX channel evaluation, pass the vertex colour through |
| `DC_PVR_NO_NEARCLIP` | drop straddling triangles instead of clipping them |
| `DC_PVR_NO_TEXTURES` | untextured backend |
| `DC_PVR_NO_TEXNULL` | restore the old behaviour where a draw with `GX_TEXMAP_NULL` still inherited `tex_handle[0]` — i.e. 2D panes sampling a stale texture's texel (0,0) |
| `DC_PVR_TEXNULL_STAGE0_ONLY` | restore the **stage-0-only** `GX_TEXMAP_NULL` test (the 2026-08-02→08-04 behaviour). The test now asks whether ANY active stage binds a texmap. The old one dropped the texture for the one emu64 combiner that parks `GX_TEXMAP_NULL` on stage 0 and carries the image on stage 1 (`emu64.c:1771-1772`); its six display-list sites in all of `src/` are **all** in `obj_s_shop1.c`, so the symptom was "Tom Nook's shop draws untextured" |
| `DC_PVR_PT_NEAREST` | ⚠️ **OPT-IN experiment.** Point-sample (`PVR_FILTER_NONE`) the punch-through list only. Tests `kb/station-bugs.md` §2 H2: the cutout palettes have no mid-alpha entry, but bilinear MANUFACTURES one at every transparent/opaque boundary and the PT comparator then straddles it. Trades a frayed cutout edge for an aliased one on every leaf and fence in town — judge it on a screenshot pair, never on counters |
| `DC_PVR_NO_ALPHATEST` | restore the old cutout handling: alpha-tested batches keep `src=ONE dst=ZERO`, so fully transparent texels paint at full opacity and write depth |
| `DC_PVR_NO_COLORMASK` | ignore `GXSetColorUpdate(GX_FALSE)`; depth-only passes paint solid geometry again |
| `DC_PVR_ALPHAENV` | ⚠️ **OPT-IN — measured to regress, off by default.** Gives a batch whose GX stage-0 alpha combiner is exactly `(ZERO, ZERO, ZERO, TEXA)` — 78 % of display-list sites, 67 % of runtime batches — `PVR_TXRENV_MODULATE` instead of `MODULATEALPHA`, so its alpha is the texel's alone rather than `vertex.a × texel.a`. That is the GX-correct answer (the vertex alpha byte there is the `G_RM_FOG_SHADE_A` **fog coefficient**, and this port fogs in PVR hardware), and it *does* clean up the dialogue balloon — but a 320×240 A/B over two 600 s runs shows the **train station canopy collapse to a flat teal slab**. Counters pass; the screenshots do not. Diagnosis and next experiment are in the comment at `alpha_env_texel_only()`. `[DC/PVR] alphaenv texel_only=` reports how often it fires |
| `DC_PVR_TEVP3` | **OPT-IN — wires the PVR's OFFSET COLOUR, which has never been used.** Fixes TEV class **P3**, of which the visible instance is the **name-entry keyboard rendering black**: 18 of its 26 display lists are config #037, `gsDPSetCombineLERP(PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, 0,0,0,TEXEL0)`, and `dc_pvr.c` implemented no part of it — `tev_const_color()` rejects at its first test, `tev_carg_affine()` rejects `GX_CC_TEXC`, so **`-DDC_PVR_TEVFOLD` provably cannot fix it**, and `pv.oargb` was hardcoded `0`. The stage is `ENV + (PRIM−ENV)·T0`; the PVR computes exactly that in one pass if the vertex base colour is `PRIM−ENV`, `pvr_vertex_t.oargb` is `ENV`, and `pvr_poly_cxt_t.gen.specular` (= `PVR_TA_CMD_SPECULAR`, BIT(2) of the PCW) is set — the hardware ADDS `oargb` after the texture env. Per-vertex cost ≈ 0; `+512 B` of `.bss` for the wider vertex memo. ⚠️ **Exact only where `PRIM ≥ ENV` per channel**: `argb`/`oargb` are unsigned bytes, so `PRIM−ENV` clamps at 0 and that channel flattens to ENV (`kai_sousa.c:520-521` is a live instance). `[DC/PVR] tevp3 batches= clamped=` reports both — **`batches=0` on a run that reached the keyboard falsifies the diagnosis outright**. ⚠️ Handles **9 of the 27** P3 configs exactly plus the P2 `a==ZERO` arm; the other 12 scale by RASC (per-vertex) and #077 uses T1. Needs a screenshot pair. `kb/tev-map-hard-cases.md` §6.6 |
| `DC_PVR_TEVP3_NOP2` | refuses `DC_PVR_TEVP3`'s `color_a == GX_CC_ZERO` arm (the P2 shape `PRIM·T0`, 3 more of the same widget's display lists at `kai_sousa.c:387,396,406`), so "P3 restored" stays separable from "P2's constant restored" in one A/B |
| `DC_PVR_P3_MODULATE` | with `DC_PVR_TEVP3`, additionally gives P3 batches `PVR_TXRENV_MODULATE` when their alpha is TEXEL0-only. This is `DC_PVR_ALPHAENV`'s change narrowed from every draw in the game to 27 configs — the global form measured a regression. Off by default; the RGB half is the black-panel fix and does not need it |
| `DC_PVR_P3_CLRCLAMP` | with `DC_PVR_TEVP3`, sets `cxt.gen.color_clamp`. ⚠️ **Probe only.** KOS calls that bit `gen.color_clamp` in `pvr.h:178` and `fog_clamp` in `pvr_header.h:285` and documents the semantics of neither. It cannot matter to the current predicate (`base + offset = PRIM ≤ 1` for any texel), so this exists to be measured, not depended on |
| `DC_PVR_NO_PUNCHTHRU` | **the punch-through kill switch.** Disables `PVR_LIST_PT_POLY` (`opb_sizes[4]` back to `PVR_BINSIZE_0`), so no batch is deferred and every cutout goes back through the 2026-08-02 blend approximation in the single general list. Restores the pre-punch-through behaviour verbatim |

#### Punch-through tuning (all imply punch-through is ON)

The PVR has no alpha test outside `PVR_LIST_PT_POLY`, and that list is number 4
— last — so cutout geometry is transformed at submission time into a deferred
32-byte-record buffer and replayed after the general list closes. See the
"decision 1" block at the top of `dc/src/dc_pvr.c`.

| define | default | effect |
|---|---|---|
| `DC_PVR_PT_ALPHA_REF` | `144` | the global `PT_ALPHA_REF` register (`0xA05F811C`), pinned to emu64's `tex_edge_alpha` default. It is ONE value for the whole render — the PVR has no per-polygon reference |
| `DC_PVR_PT_BUF_RECS` | `2048` | deferred records, 32 B each = **65,536 B of `.bss`**. Raise it if `[DC/PVR] … ptdrop=` is ever nonzero |
| `DC_PVR_PT_BINSIZE` | `32` | PT object-pointer bin size. VRAM only, ~153,600 B per buffer set. `16` halves it |
| `DC_PVR_PT_ALL` | unset | also route **blended** cutouts (`GX_BM_BLEND` + alpha test) to PT. By default only `GX_BM_NONE` cutouts — opaque-with-holes — go there, because PT forces a passing fragment's alpha to 1.0 and would make a fading sprite pop opaque |

The `[DC/PVR] pt …` line printed next to `[PERF]` every 30 frames carries
`batches` / `verts` / `recs` routed to PT, `pthi=<worst frame>/<cap>` and
`ptdrop=<triangles refused by a full buffer>`. **`ptdrop` must be 0**; anything
else is cutout geometry missing from the screen. `DC_PVR_BATCH_LOG` gained a
`pt=` field next to `cut=`: `cut=1 pt=0` is a cutout the router declined.

⚠️ **A kill switch reverts one fix, not one symptom.** `DC_PVR_NO_UVCLAMP`
built to test the train windows also un-fixes K.K. Slider's spotlight, because
that is what the wrap fix repaired. Say what a given A/B is expected to break
before anyone looks at it.

```bash
DC_XDEFS='-DDC_PVR_NO_UVCLAMP' bash dc/build-dc.sh
```

```bash
DC_TARGET=objs bash dc/build-dc.sh     # compile-only
DC_CDI_PAD=1   bash dc/build-dc.sh     # CD-R burn image
JOBS=8         bash dc/build-dc.sh
DC_OPT_PROFILE=o0 bash dc/build-dc.sh   # the -O0 kill switch
DC_ASSET_STUB=1 bash dc/build-dc.sh    # bring-up image that actually boots
```

The image that currently gets furthest — past the title, into the train intro,
with real textures — is a stub image with a censused keep list and autostart:

```bash
python3 tools/dcstub/census_resolve.py <run>/console.log \
    --sizes-from dc/build/nonstub/AnimalCrossing.elf --top 0 > /tmp/census.txt
DC_STUB_KEEP="$(python3 tools/dcstub/census_keeplist.py /tmp/census.txt \
                 --with-default --colon)" \
DC_DISC_ROOT=~/.cache/oc-dc-discroot DC_ASSET_STUB=1 \
DC_ARAM_WINDOW=131072 DC_ARENA_BYTES=1900000 DC_AUTOSTART=300 \
  bash dc/build-dc.sh
```

### `DC_ASSET_STUB=1` — the bring-up image

The real image is 8,273,108 B over 16 MB, so it never executes an instruction:
startup `.bss` zeroing runs off physical memory before `scif_init()` and there
is not even console output. `DC_ASSET_STUB=1` runs
`tools/dcstub/make_stub_data.py` on the host first, which rewrites every asset
destination array to `[1]` (2,535 TUs, 16,317 arrays, 8,716,158 B) into
`dc/build/stubsrc`; `dc/Makefile` then compiles those TUs instead of their
`src/` originals. `src/` is untouched — the arrays are generator output, so
generating them small is a generator change, legal under the `-O0` rule
(CLAUDE.md §1).

The build also defines `-DDC_ASSET_STUB`, which makes `dc_main.c` skip
`pc_assets_init()` (the central table would memcpy full-size assets over
one-element destinations) and defaults `g_pc_verbose` to 1 (every `OSReport` in
the game is gated on it, and a burned CD-R passes no argv, so `--verbose` is
unreachable there). `-DDC_VERBOSE` turns the latter on by itself for a normal
build.

The game renders garbage the moment it reads an asset. That is the point: the
image exists to exercise the platform layer, not to look like Animal Crossing.
`bash dc/build-dc.sh clean` removes the stub tree with the rest of `dc/build`.

### `DC_DISC_ROOT` — putting real game data on the disc

Without it the CDI is built from the ELF alone, `/cd` mounts empty and every
`DVDFastOpen` misses — which is where the S1 image stops, in
`JKRAramArchive::open()` on a zero-byte `forest_1st.arc`.

`dc_dvd.c` builds every path as `"/cd" + "/" + name` (`dc/src/dc_dvd.c:113`),
so the files must sit at the **disc root, flat**. `dcasset extract` writes the
GameCube shape instead (`files/`, `sys/`), so it needs flattening first:

```bash
python3 tools/dcasset/dcasset.py extract "<the ISO>" --out /tmp/discroot
bash dc/stage-disc.sh /tmp/discroot /tmp/discflat        # 11 files, 36,953,162 B
DC_DISC_ROOT=/tmp/discflat DC_ASSET_STUB=1 bash dc/build-dc.sh
```

The directory is bind-mounted read-only at `/discroot` and passed to
`mkdcdisc -D`. **Keep the staging directory out of the repo** — it is ROM
material, and neither it nor the CDI may ever be committed (CLAUDE.md §1).

⚠️ `pc_disc_extract_rel()` (`dc/src/dc_dvd.c:290`) reads the whole 15,640,056 B
`foresta.rel` into RAM. On a 16 MB machine that cannot work; it is exactly what
`kb/levers.md` L2 replaces with `assets.pak`. It is only reachable from
`pc_assets_init()`, which the stub build skips — so the stub image is safe, and
a non-stub image with disc content is not.

---

## Padded vs unpadded CDI

Measured (`kb/design-toolchain.md` §5.2):

| | size | time |
|---|---|---|
| `mkdcdisc -N` (default here) | 1,783,337 B | 0.021 s |
| `mkdcdisc` (padded) | 740,083,145 B | 15.6 s |

The default is `-N`, because 740 MB per iteration would make the Flycast loop
untenable. The padding is not waste — it pushes content toward the outer
tracks, which is what a real CD-R wants. **Use `DC_CDI_PAD=1` for anything
you burn, and for any timing run that has to be read-speed-realistic.**
Streaming numbers measured against an unpadded image are optimistic.

---

## Optimization — `DC_OPT_PROFILE`, and it is now the biggest lever in the build

⚠️ **REVERSED 2026-08-06 by user decision.** This section used to say raising
`DECOMP_OPT` was banned. It is not. Measured on this tree, `-Os` cut `.text`
by **2,826,288 B** and took the town from **11.6 to 18.5 FPS**; a 14-TU `-O3`
hot list took it to **20.0**. The post-mortem on why the ban stood — armhf
evidence, never reproduced on SH-4, never isolated from a simultaneous NEON
change, most likely a link bug — is in `kb/closed.md`.

```bash
bash dc/build-dc.sh                        # perf profile: -Os + -O3 hot list
DC_OPT_PROFILE=size bash dc/build-dc.sh    # -Os everywhere, for when it won't fit
DC_OPT_PROFILE=o0   bash dc/build-dc.sh    # THE KILL SWITCH: byte-identical revert
```

| profile | decomp default | hot list | when |
|---|---|---|---|
| `perf` (default) | `-Os` | `-O3` | normal builds |
| `size` | `-Os` | `-Os` | the image is over budget; trades frame time for `.text` |
| `o0` | `-O0` | `-O0` | bisecting a suspected miscompile; the guaranteed-good state |

**The two lists live in `dc/opt-lists.mk`**, each entry with its evidence:

* `OPT_HOT_SRC` — 14 TUs at `-O3`. Costs **+48,476 B** of `.text` over flat
  `-Os` and buys 3.5 ms. It is short on purpose: `emu64.c` is most of the
  frame, and **every other `src/` TU shares the ~2.8 ms logic tick**, so a
  perfect 2× on all of them together is worth well under 1 FPS.
* `OPT_QUARANTINE_SRC` — TUs forced to `-O0` because they are *measured* to
  miscompile. Empty today.

**A list entry that matches no TU in the build is a hard error**, not a silent
no-op — the same failure mode that left G1 unrun for two sessions.

`DECOMP_OPT` / `DECOMP_HOT_OPT` in the environment still override the profile.
`-O0` there literally means `-O0`, not "omit `-O`": `$KOS_CFLAGS` already
carries `-O2` and the last `-O` on the command line wins.

### The guard set is what makes this legal

`UB_GUARDS` (unchanged) plus `OPT_GUARDS`, which is empty at `-O0`:

| flag | why |
|---|---|
| `-fno-isolate-erroneous-paths-dereference` | stops GCC turning a tolerated NULL deref into a trap — the exact shape of the armhf "wild pointer from boot" report |
| `-fno-ipa-sra` | the decomp defines 968 functions with K&R `()` parameter lists and calls some of them WITH arguments (`m_camera2.c:226`, called `(play)` at `:258`). IPA-SRA rewrites exactly those calling conventions |
| `-fno-store-merging` | SH-4 traps *any* misaligned `mov.w`/`mov.l`; there is no `-mno-unaligned-access` on this target |
| `-fno-ipa-icf` | keeps a crash address attributable to one function, so `harness/dc/crash.sh` stays useful |

### When an optimized image misbehaves

Work down this list; the first two are single builds at 96 s each.

```bash
DC_OPT_PROFILE=o0 bash dc/build-dc.sh        # 1. is it optimization at all?
DC_AUTOVAR_INIT=zero bash dc/build-dc.sh     # 2. is it an uninitialised read?
                                             #    (99 of them; -O0 and -Os
                                             #     disagree about their value)
DC_OPT_O0_EXTRA='src/static/libforest/emu64/emu64.c' bash dc/build-dc.sh
                                             # 3. is it the display list?
DECOMP_HOT_OPT=-O2 bash dc/build-dc.sh       # 4. -O3 is unproven on emu64.c;
                                             #    -O2 on it is device-verified
```

Then bisect properly:

```bash
DC_TARGET=warnscan bash dc/build-dc.sh                 # ~132 s, all TUs at -O2
python3 tools/dcopt/warnscan_report.py dc/build/warnscan.log
python3 tools/dcopt/warnscan_report.py dc/build/warnscan.log --paths > /tmp/cand.txt
bash tools/dcopt/bisect_o0.sh /tmp/cand.txt
```

`warnscan` recompiles every TU at `-O2` with the decomp's `-w` removed, into a
throwaway objdir it then deletes. It exists because the two diagnostics that
predict a miscompile are silenced by construction in a normal build: a missing
return (35 of them, and in the four TUs compiled as C++ that is a path G++
DELETES) and an uninitialised read.

**A culprit found by bisect goes into `OPT_QUARANTINE_SRC` with its evidence —
the symptom, the run directory and the date — not back into a tree-wide `-O0`.**

**Gate on any raise, unchanged from the old section:** a full new-game intro
(K.K. → train → town arrival), and a screenshot pair. `run_report.py` is the
floor and cannot see colour.

---

## How the flags are put together

`kos-cc` / `kos-c++` prepend `$KOS_CFLAGS` (`-ml -m4-single`, KOS include
paths, `-ffunction-sections -fdata-sections`, `-O2`, `-g`). The Makefile's own
flags come **after**, so they win on conflicts. Never call `sh-elf-gcc`
directly — the wrappers are what keep us ABI-compatible with the prebuilt
`libkallisti` / newlib / `libstdc++`.

Per-TU language handling mirrors `pc/CMakeLists.txt`:

```
decomp C (default)                        -w -std=gnu89 -fpermissive
jaudio_NES/*, libforest/emu64/*           -w -std=gnu11 -fpermissive
emu64.c, ja_calc.c, jammain_2.c, game64.c compiled as C++ (-x c++)
decomp C++                                -w -fpermissive -fno-exceptions -fno-rtti
dc/src/*.c                                -O2 -std=gnu11 -Wall -Wextra
src/main.c                                -Dmain=ac_entry     (KOS owns main)
src/static/boot.c                         -Dmain=boot_main
```

UB guards on all decomp code: `-fno-strict-aliasing -fwrapv
-fno-delete-null-pointer-checks -fno-lifetime-dse
-fno-aggressive-loop-optimizations -fno-strict-overflow -fno-stack-protector
-fsigned-char`. Each is justified per-flag in `kb/design-shelf-hazards.md`
§3.1 — with one correction: **`-fno-builtin` is deliberately not used.** §3.1
calls it "KOS convention (`KOS_CFLAGS`, VERIFIED)", but this image's
`$KOS_CFLAGS` does not contain it, and it breaks the link: it stops GCC
expanding `__builtin_alloca` inline, so `src/game/m_select.c:936,993` emit
calls to a real `alloca` symbol that newlib does not provide. There is no
`-fbuiltin-alloca` to re-enable it selectively.

`-DTARGET_PC` is **non-negotiable**. It means "not GameCube", not "PC": it
guards the base port's little-endian correctness fixes (byte-wise texconv in
`emu64.c`, the swapped `u16` pair ordering in `sys_matrix.c`, the overlap-safe
`Jac_bcopy` in `sample.c`). `-DTARGET_DC` is added alongside it for branches
that are genuinely Dreamcast-only.

---

## Include-path order is load-bearing

```
-Idc/include  -Ipc/include  -Iinclude  -Isrc  -I.
```

`dc/include` **must** come first so that:

* `dc/include/pc_platform.h` (an SDL-free shim) wins over the real
  `pc/include/pc_platform.h`, which pulls `<SDL.h>` and `<sys/mman.h>`. Five
  decomp TUs include it directly: `src/graph.c`, `src/game.c`,
  `src/game/m_play.c`, `src/game/m_actor.c`,
  `src/static/libforest/emu64/emu64.c`.
* `dc/include/SDL.h` (six symbols, not SDL) satisfies
  `src/static/jaudio_NES/internal/os.c`'s `#include <SDL.h>`.

`include/libc` is deliberately **not** on the path — having both `include/` and
`include/libc/` breaks the decomp's `#include_next` shadow-header chains, the
same as on PC.

---

## `dc/include/dc_prelude.h`

Force-included into every TU with `-include`. It exists because several
KOS/newlib identifiers collide with decomp identifiers in ways no include
ordering can fix (the decomp reaches `arch/arch.h` transitively through
`include/dolphin/types.h` → `<stdio.h>`). It handles exactly four things:

| Collision | Fix |
|---|---|
| `arch/arch.h:34` `#define page_count …` vs `u8 page_count;` in `m_notice_ovl.h`/`m_address_ovl.h` | pull `arch/arch.h` first, then `#undef page_count` |
| `<unistd.h>` `int link(const char*, const char*)` vs `typedef struct link_ link;` in `audiostruct.h` | rename **only** the POSIX declaration while pulling `<unistd.h>` in, then give the identifier back |
| KOS `dc/fmath.h:109` `static inline float fsqrt(float)` vs `math64.h:34` `#define fsqrt(x) sqrtf(x)` (which would rewrite KOS's *definition* into a static `sqrtf`) | include `dc/fmath.h` before any decomp header can define the macro |
| newlib's C++ headers don't pull `<string.h>` transitively (glibc's do) — breaks `JUTFont.h` / `JFWDisplay.cpp` | `#include <string.h>` |

Do not put game declarations in it.

---

## `pc/src` files compiled into the Dreamcast build

Three of them, listed in `PC_REUSE_C` in `dc/Makefile`. They are
platform-independent logic sitting *above* a backend that `dc/src` supplies,
classified as reusable by `kb/design-platform-api.md` §2a/§2b:

| file | why it is here |
|---|---|
| `pc_assets.c` | 30 677 LOC generated asset dispatch table. Its `pc_disc_*` backend is implemented in `dc/src/dc_dvd.c`. |
| `pc_save_bswap.c` | GCI byte-swap tables; §2a "truly verbatim". |
| `pc_m_card.c` | re-implements the game's own `src/game/m_card.c`, which **both** builds exclude. Without it the link is short 29 `mCD_*` / `pc_save_*` symbols. It compiles clean for sh-elf as-is, but **compiling is not working**: its `.gci`-file backend still has to become VMU/vmufs (PLAN §6 — ~100 KB of VMU for a ~456 KB GC save). |

`pc_settings.c` is *not* included: it uses `SDL_DisplayMode` at
`pc_settings.c:366-385`. `pc_disc.c` is not either — §2b moves it to `tools/`
as host code.

---

## Where this build currently stands

`make all` links and `mkdcdisc` produces a CDI. **It does not boot — it is too
big to load.**

```
   text      data       bss   image span   image end
6318552   2638852  12415508   21374068   0x8d472874
```

KOS's `_arch_mem_top` for a stock console is `0x8d000000`, so the image ends
past the top of RAM before any heap is touched.

`harness/dc/smoke.sh dc/build/OpenCrossing.cdi` returns `timeout` with **zero
bytes of console output** — no KOS banner at all. Attributed by experiment
(`kb/mem-budget.md` §8.7): a KOS hello-world containing nothing but a 21 MB
`.bss` array fails identically at essentially the same image end, while the
same hello-world with 4.7 MB (end under `_arch_mem_top`) passes in 3.08 s, and
the harness's own `selftest.cdi` passes in 3.10 s. **The guest never executes
an instruction, so there is no PC to symbolise**, and
`-DDC_NO_CRASH_PROTECTION` cannot distinguish anything here. Nothing about the
port's correctness is testable until the image fits.

Being under `_arch_mem_top` is **not** the bar. KOS's `mm_sbrk()` starts at the
ELF `end` symbol, with no MMU and no lazy commit, so every `.bss` byte destroys
a heap byte. The fit is **one inequality**, and stating it as two pools has
already produced two wrong numbers:

```
(image span) + (genuinely additive heap) ≤ 16,646,144
  21,374,068  +  3,545,184   ⇒ over by 8,273,108 B
```

All of it has to come out of **layout, not codegen** — see the optimization
section above.

**Live numbers: `kb/STATE.md`. The ranked levers: `kb/levers.md`. What is
already ruled out: `kb/closed.md`** — read that one before proposing anything
here. This section deliberately does not duplicate them; it went stale twice
when it did.

---

## Adding a source, or excluding one

Sources are discovered by `find` + a single ERE of exclusions, reproducing all
35 filters from `pc/CMakeLists.txt` verbatim. `dc/src/*.c` is globbed, so a new
platform file needs no Makefile edit.

`src/` is **vendored decomp**. When a TU genuinely cannot compile for SH-4, add
it to `DC_EXCLUDE_C` / `DC_EXCLUDE_CXX` in `dc/Makefile` with a reason and a
date — do not hack `src/` in place. Prefer a shim in `dc/include/` over an
exclusion; every one of the 12 known sh-elf failures
(`kb/design-shelf-hazards.md` §2) is handled by a shim, not an exclusion.

---

## Troubleshooting

**`docker image 'opencrossing-dc:sdk' not found`** — run
`bash dc/build-dc-image.sh`. Stage 1 (the toolchain) is ~24 min and is cached
forever; stage 2 is ~2.5 min.

**`No rule to make target 'build/obj/…'`** — the makefile's object paths are
absolute. Ask for `/work/dc/build/obj/src/foo.c.o`, not a relative path.

**Crash on hardware / in Flycast** — build keeps `-g` (free at runtime, and the
ELF never ships on the disc; only `1ST_READ.BIN` does). Feed the reported PC to
`sh-elf-addr2line -f -e dc/build/AnimalCrossing.elf <pc>`. For alignment faults
specifically see `kb/design-shelf-hazards.md` §4 — SH-4 traps every misaligned
16/32-bit access, a bug class no previous port of this codebase has ever
exercised.
