# Next session — the seed prompt, and the state it assumes

**Written 2026-08-12, at the end of the session that got gprof running on real
silicon.** This file exists to survive a context flush. It is NOT a second copy
of `kb/RESUME.md` — it is the *paste-me* prompt plus the handful of facts that
are only in one human's head right now.

---

## 1. The prompt to paste

> Read `CLAUDE.md`, then `kb/RESUME.md` (especially §6b and §6c), then
> `kb/STATE.md`. We got KOS `libgprof` running on a retail Dreamcast on
> 2026-08-12 and it re-ranked the whole perf queue: **audio is ~21 % of real
> work and 2.18× its Flycast share, while `dc_gx_backend_submit`'s share
> SHRANK on hardware — the icache prediction in `kb/hardware-profiling.md` §7
> is falsified.** Two fixes shipped that day (`DC_RSPSIM_NOFP`, `dc_ctz32`).
>
> The profile we have is the **title screen's demo scene**, not the walked
> town, so every vertex-load-dependent number in it is provisional. **The next
> measurement is a hardware TOWN profile**: burn the `DC_GPROF_SD=1` build,
> play to the town, walk a minute, stand still, press **L+R+START**, and bring
> the SD card back. Then `sh-elf-gprof -b -p <matching .elf> gmon.1`.
>
> Do not quote FPS from any `DC_GPROF` build, and do not compare a
> `keeplist-town` build's frame rate to a `keeplist-full` one.

---

## 2. What only exists in a human's head

- **The SD adapter is the SCIF type** (jj1odm-style), confirmed by
  `iface SCIF` in `/sd/gprof.txt`. `DC_GPROF_SD_IF` defaults to 0 for that
  reason. ⚠️ **Do not set it to 1 or 2** — KOS's `sci_spi_rw_byte` waits on RDRF
  with no cycle cap and wedges forever with the console already muted.
- **The card must be MBR with the FAT32 volume in PRIMARY entry 0.** The first
  card failed because macOS had it as a *logical* partition inside an extended
  partition (`diskNs5`, offset 2112 = 2048 + an EBR).
  `diskutil eraseDisk FAT32 DCLOG MBRFormat /dev/diskN` produces the right
  layout; 4 KB clusters matter because `fat_fs_init_ex` allocates 8
  cluster-sized cache buffers.
- **The disc the human has been playing is the PROFILING build**, which is
  handicapped four ways against a play build: F5 off, gprof sampler armed,
  console unmuted, `keeplist-town`. A human read it as *"running noticeably
  better"* — ⚠️ **that is the missing content, not a win.**
- **Holding L auto-advances dialogue** under `TARGET_PC` (`kb/traps.md`). It is
  the difference between reaching Tom Nook and giving up.
- **The title screen runs a live demo scene** — actors, camera, music. That is
  why the profile has `Player_actor_move` and `Camera2_*` in it.

---

## 3. The artefacts, and where they are

On the NAS at `/Volumes/Gabe/AC-DC/` (durable) — `dc/build/gprof-runs/` has the
same files locally but **`make clean` does `rm -rf dc/build`**:

| file | what |
|---|---|
| `AC-DC-20260812b-gprof-sd.{cdi,elf,cdi.src.json}` | the hardware profiling image. ⚠️ **Keep the `.elf`** — `sh-elf-gprof` needs the exact one, checked against the sidecar sha256 |
| `…hw-title-1958f.{gmon.out,flat.txt,gprof.txt}` | the good hardware run |
| `…hw-title-69f.*` | the 69-frame run — **boot-dominated**, useful only as the phase-subtraction arm |
| `AC-DC-20260812a-gprof.*` | 🔴 **pre-fix, do not burn.** Its SD path cannot write |

⚠️ The card file is opened `"w"`, so **every run overwrites `GMON.1`**. Copy it
off before the next run.

---

## 3b. ⚠️ THE NEXT BURN IS THE TOWN PROFILE, AND THAT IS A DECISION

**User directive, end of session 16b: do NOT burn the play build.** A
`keeplist-full` + F5-on + no-gprof build carrying P3 and `dc_ctz32` was built and
passed the regression gate (`no regression detected`, `ASSET MISSING 0`,
`fps_p50` 22.8 unchanged), but it was **deliberately not shipped** — the two
fixes are ~2.4 % of CPU and a CD-R is better spent on the measurement that
unblocks everything else.

It is NOT on the NAS. It is at `scratchpad/play.{cdi,elf,cdi.src.json}` and that
scratchpad does not survive; **rebuild it from the shipping config in
`kb/RESUME.md` §2** (nothing extra is needed — P3 and `dc_ctz32` are defaults).

⚠️ **`fps_p50` unchanged in Flycast is NOT evidence the fixes did nothing.**
Audio is 9.4 % of busy time in Flycast against 23.9 % on hardware, so the
emulator understates P3 by ~2.5×. Measurement rule 12.

---

## 4. The three things to do next, in order

1. **A hardware TOWN profile.** Everything vertex-load-dependent is provisional
   without it: G-B's 9.34 %, the frustum test's 30 %-of-G3, and
   `setup_1tri_2tri_1quad`'s 0.6 %. Same disc, `DC_CONSOLE_MUTE=1` next time.
2. **Run N3 (`DC_NPCDIAG=1`).** The profile shows ~22 distinct `aNPC_*` procs
   with samples and `aNPC_dma_draw_data_proc` at **zero** — the first evidence
   the villager actors may EXIST and the break is inside DRAW, which is a
   different branch of `dc_npcdiag.c`'s decision table than the kb has been
   aiming at. ⚠️ It may be the demo's villagers, not the player's.
3. **Decide on audio.** Cheap and measured: the reverb bus mixes 2.17× more
   samples than the engine produces (`DMEM_2CH_SIZE` is sized for 48 kHz;
   `DC_AUDIO_MIXRATE=24000` makes 96 samples, the constant says 416). Expensive
   and now justified: AICA stage B, which deletes `RspStart` outright.

⚠️ **`DC_AUDIO_VOICES`, `DC_AUDIO_MIXRATE` and `DC_AUDIO_SUBDELAY` are already
applied.** They are spent. Do not re-propose them as new wins.

---

## 5. The instrument's own traps, so nobody re-learns them

- **Samples accrue per RESCHEDULE, not per timer tick**, while the gmon header
  claims `profrate = thd_get_hz()`. Idle is structurally over-represented and
  `thd_block_now` is the sampler counting its own trigger — **neither is work**.
  Use non-idle, and say which denominator you used.
- **There is no call graph** and there never will be: `-pg` is on the link line
  only, by design. Flat self time only; do not infer callers. A gmon.out from
  this port has **zero bytes** after the histogram record.
- **`fopen(…,"a")` is unusable on `fs_fat`** — `O_APPEND` sits inside
  `O_MODE_MASK`, so every append write returns `EBADF` while the open succeeds.
- **F5's `section-order.txt` is link-specific.** With `keeplist-town` it hangs
  Flycast before `main()`; hardware boots it anyway. Profiling builds run
  `DC_SECTION_ORDER=0`.
- **A wedge behind a muted console is indistinguishable from a game hang.** It
  cost four runs and two wrong diagnoses in one session.
