# Boot blockers — what the running game hits next

> ⚠️ **UPDATED 2026-08-02 (later).** Items **2, 4 and 9 are DONE** —
> `DC_AUTOSTART` presses the button, the console flood is rate-limited, and
> `OSGetSoundMode()` returns stereo. The game now reaches the **train intro**
> (player-select scene). Item **7 (TEV) moved up**: with real geometry on
> screen, a cull-mapping bug that made every character render inside-out was
> found and fixed (`kb/traps.md` → Renderer), and TEV is the next thing between
> the port and a correct-looking frame. **Item 5's premise is WRONG** — see the
> correction at the end of §3.4 and in `kb/STATE.md`. Everything else below
> still stands.

Written 2026-08-02. A **read-only audit**: no code was changed to produce it.
It answers one question — *as the port gets further, which stub does the game
reach first?* — and ranks by **reach**, not difficulty. Counterweight to
`kb/levers.md`, which ranks by bytes.

**Evidence.** One console log,
`~/.cache/oc-dc-harness/runs/smoke-OpenCrossing-20260802-091704-87290/console.log`
(3,956 lines), plus `dc/build/AnimalCrossing.map` (read 10:17; the build tree is
live) and a read of every file in `dc/src/`. Claims are tagged **[traced]** or
**[inferred]** — `CLAUDE.md` §5 exists because that distinction has been lost
here before. **Out of scope:** the 5,431,104 B RAM gap and the S4 loader;
everything below assumes they land.

---

## 1. Where execution actually is

`kb/state-log.md`'s boot list is accurate but stops one step early. The game
does **not** halt at the save scan:

```
180  [PC] No save file found
191  [PC] trademark_init: enter
195  [LOGO] aAL_actor_ct: Animal Logo actor created
...  [LOGO] draw: action=3 ... press_start_opacity=255 / 0 / 143 / 255 ...
```

It passes the scan, enters the title demo, and **loops forever in
`aAL_game_start_wait` (`src/actor/ac_animal_logo.c:245`) waiting for START or
A**. `padmgr_isConnectedController(PAD0)` is true, `mLd_StartFlagOn` has run,
`famicom_mount_archive_end_check()` returns 1 — every gate at
`ac_animal_logo.c:268-273` is open except the button. [traced]

The frontier is not a crash and not a stub. It is an **unpressed button**.

---

## 2. The ranked list

Ordered by when the running game reaches it. "Now" means it is firing in the
log above.

| # | when | blocker | where | on hit | size |
|---|---|---|---|---|---|
| 1 | **now, every frame** | audio production pipeline is disconnected — nothing drains jaudio's command queue | `dc/src/dc_audio.c:116` (announced) + no caller for `pc_audio_process_frame` | silence; 3,374 log lines/run | L (gated by a decision) |
| 2 | **now, every frame** | console flood: 85 % of the log is one line | `dc/src/dc_os.c:606` `OSReport` | every future log is unreadable | S (~20 lines) |
| 3 | **now** | ARAM out-of-window traffic; all game *text* lives there | `dc/src/dc_aram.c:205` | zero-filled reads → blank dialogue | **IN FLIGHT** |
| 4 | **the next step** | no way to press START unattended | `harness/dc/_runner.py` (none) / `dc/src/dc_pad.c:36` | nothing past the title, ever, in CI | S (~30 lines) |
| 5 | title → player select | VMU / `CARD*` | `dc/src/dc_card.c:174,408` | **IN FLIGHT** | — |
| 6 | first scene load | no DVD read-ahead; 4,132,608 B archive at ~500 KB/s | `dc/src/dc_dvd.c:182` | ~8 s freeze per load on real CD-R | M |
| 7 | first non-logo geometry | 1 of 101 TEV configs implemented | `dc/src/dc_pvr.c:502` | wrong colours, silently | M–L |
| 8 | **first menu press in town** | EFB→texture capture | `dc/src/dc_gx.c:1620` + `:1709` | every menu draws over black, not the frozen world | M |
| 9 | first sound-mode read | `OSGetSoundMode()` returns 0 = **mono**, contradicting the audio plan of record | `dc/src/dc_stubs.c:118` | game locks itself to mono | **1 line** |
| 10 | first save | `dc_vmu_write_file` | `dc/src/dc_card.c:174` | **IN FLIGHT** | — |
| 11 | first options change | settings never persist | `dc/src/dc_misc.c:403` | silent no-op | S |
| 12 | fade to black transitions | `VISetBlack` is a no-op | `dc/src/dc_vi.c:251` | minor visual | S |
| 13 | player obtains an NES | `famicom_init` | `dc/src/dc_stubs.c:153` | documented non-goal, degrades gracefully | — |
| 14 | any rumble event | `PADControlMotor` | `dc/src/dc_pad.c:120` | cosmetic; already hit at boot | S |
| 15 | never (stale) | `__OSSetExceptionHandler` still announces itself although crash recovery **is** implemented elsewhere | `dc/src/dc_os.c:786` vs `dc/src/dc_main.c:204` | misleading `[DC/TODO]` | S (delete it) |

Silent no-ops below the fold, all cosmetic, all traced to zero or one live call
site: `GXSetDither` (`dc_gx.c:1252`), `GXSetCoPlanar` (`:1270`), `GXSetClipMode`
(`:979`), `GXSetDispCopyGamma` (`:1586`), `GXInvalidateTexAll` (`:1920`),
`pc_typing_queue_pop` (`dc_stubs.c:207`), `pc_model_viewer_init` (`:214`),
`pc_overlay_*` (`:223-230`), `PPCMfmsr`/`PPCMtmsr` (`dc_misc.c:158-159`,
reachable only *after* a fault), `DVDCheckDisk` (`dc_stubs.c:67`, shutdown only).

---

## 3. The blockers, one paragraph each

### 1. Audio is not stubbed at the device — it is disconnected two levels up

`dc_audio.c:116` announces the missing `snd_stream`, and that announcement is a
decoy. The real state is worse and is **[traced] in the map file**:

```
.text.pc_audio_process_frame   0x00000000  0x68   audiothread.c.o
.text.Jac_UpdateDAC            0x00000000  0xb4   aictrl.c.o
.text.Jac_VframeWork           0x00000000  0x19c  aictrl.c.o
```

Address `0x00000000` means `--gc-sections` deleted them. `pc_audio_process_frame`
is the per-frame audio pump; on PC the SDL producer thread calls it
(`pc/src/pc_audio.c:43`), and `dc_audio.c:201`'s
`pc_audio_start_producer_thread()` deliberately starts nothing. A grep over
`src/`, `dc/` and `pc/` finds **no DC-side caller** — so the linker was right to
drop it, and `Jac_VframeWork → MixCpu → CpubufProcess(MIX)`, the whole per-frame
mix, went with it. [traced]

Hence `SendStart::Mesg Full Queue`: `Nap_SendStart` (`sub_sys.c:259`) pushes
into `AG.thread_cmd_proc_mq_p`, the only drain is `sub_sys.c:733` inside
`CreateAudioTask`, and that is reached only through the deleted mix path. The
queue fills once and stays full forever — 3,374 rejections in one run. [traced
for the count and the link addresses; **[inferred]** that the deletion is the
sole cause, though no other drain exists]

**Do not fix this by wiring the pump back in and moving on.**
`kb/audio-plan-of-record.md` §9: software synthesis costs an estimated 13 % CPU
at 10–12 voices and up to 68 % at shipped settings, and the live-voice-count
measurement (§7 item 1) has never been taken. Restoring the pump also un-deletes
`.text` the RAM ledger has not budgeted. First move is item 2, then that
measurement — on the PC build, not a DC build.

### 2. The console flood

3,374 of 3,956 lines (**85.3 %**) are `SendStart::Mesg Full Queue`; another 139
are `[WALK] BLOCKED` (`game64.c_inc:2092`, benign — `sou_scene_mode` is 0 during
the title demo). [traced] Every `OSReport` funnels through `dc_os.c:606`, which
is DC-owned, so a repeat-suppressing rate limiter there is legal under the
"never edit `src/`" rule and costs ~20 lines. It will not catch `[WALK]
BLOCKED`, which calls `printf` directly. On hardware this is also a frame-time
problem — `dc_platform.h:222` notes dbgio runs at 57600 baud.

### 3. ARAM (in flight) — but note the blast radius

One sentence beyond "in flight": **all game text is read from ARAM offsets.**
`m_string.c:14` sets `String_table_rom_start =
JW_GetAramAddress(RESOURCE_STRING_TABLE)` and `mMsg_aram_init2` (log line 172)
wires the message tables the same way, so with `dc_aram.c:198`'s zero-fill every
line of dialogue, every sign and every letter reads as zeros. [traced] The log
already shows out-of-window traffic at `trademark_init` — before the logo draws.

### 4. Nothing can press START

`smoke.sh` has no input flag and `_runner.py` has no input path — `maple`
appears there only as a log parser (`_runner.py:243`). [traced] So no unattended
run can observe items 5–14. Two options: a Flycast-side input script, or — much
cheaper and testable on hardware too — a `DC_AUTOSTART=<frames>` knob that makes
`PADRead` (`dc_pad.c:36`) synthesise `PAD_BUTTON_START` for a few frames at a
chosen count. ~30 lines, kill-switch by construction (absent knob = today).

**DONE 2026-08-02.** `DC_AUTOSTART=<N>` in `dc_pad.c`, exactly as sketched:
pulses of 6 `PADRead` calls every `DC_AUTOSTART_PERIOD` (90), alternating START
and A, absent by default. It took the game from the title screen to the train
intro on its first run. ⚠️ A *faster* period (24) is worse, measured — the
dialogue needs press/release edges.

**Correction to item 5, found the same day:** the next scene does **not** gate
on the memory card. `aNPS_setup_game_start` waits on `mCD_InitGameStart_bg()`,
which in this build is `pc/src/pc_m_card.c:1188` — an override that returns
`mCD_TRANS_ERR_NONE` unconditionally, not `src/game/m_card.c:5096`'s card state
machine. Reading `src/` alone gives the wrong answer here because the link
carries `--allow-multiple-definition`; **check `pc/src` for an override before
tracing any `src/` symbol as a blocker.**

Traced negative worth banking: the game's menus do **not** use the digital
D-pad. `PAD_BUTTON_UP/DOWN/LEFT/RIGHT` over `src/` outside
`src/static/dolphin/` returns exactly **3** hits, all in `boot.c`'s zurumode
menu; `m_select.c:366` navigates with `getJoystick_Y()`. `dc_pad.c:89-92`'s
D-pad → C-stick mapping is therefore not a menu blocker. [traced]

### 6. DVD read-ahead

`dc_dvd.c:182` marks the spot; `dc_dvd.c:220` prints "NO read-ahead yet" at boot.
`forest_2nd.arc` is 4,132,608 B; at the ~500 KB/s a CD-R sustains that is **~8 s
of dead frames** per scene load. [inferred — arithmetic on the logged file size,
never timed] The emulator hides it (`smoke.sh` defaults to fast GD-ROM;
`--no-fast-gdrom` shows it), so it surfaces only on a burn. Caveat:
`dc_dvd.c:12-13`'s contract point 1 requires `DVDReadAsyncPrio` to keep firing
its callback before returning, and a real KOS reader thread re-opens
`dc_os.c:359`'s `OSDisableInterrupts` no-ops.

### 7. TEV

`dc_pvr.c:42` is honest: one configuration — modulate texture by rasterised
colour — against the 101 in `kb/tev-map.md`. Already ranked as `kb/STATE.md` N3;
it appears here only to fix its *position*, which is immediately after anything
but the logo draws — ahead of every menu and save item.

### 8. EFB→texture, and why it is the first *town* stub

`GXCopyTex` (`dc_gx.c:1620`) is a loud no-op. Its one live game call site is
`copy_efb_to_texture` (`m_play.c:668`), reached from `m_play.c:793` when
`submenu.mode == mSM_MODE_PRERENDER_INIT` — which `m_submenu.c:312` sets
whenever **any** submenu opens: inventory, map board, notice board, letters,
catalogue, every shop menu. The other trigger in that condition,
`FBDEMO_MODE_CREATE`, is **never assigned anywhere in `src/`** (only read), so
wipes and fades are ordinary geometry and are unaffected. [traced] Menus will
render over a black rectangle instead of a frozen world snapshot — degradation,
not a crash, because `prbuf` (`m_play.c:62`, 614,400 B of `.bss`) stays zeroed.

Nuance worth ten minutes before scoping: on GameCube the copy is *recorded* into
a display list and replayed. On DC `GXBeginDisplayList` (`dc_gx.c:1709`) is a
no-op and `GXEndDisplayList` returns 0, so the replay never happens — but
`GXCopyTex` is still called inline at record time. If that holds, implementing
`GXCopyTex` alone suffices and the display-list recorder can stay dead.
**[inferred]**

### 9. The stub that lies: `OSGetSoundMode()`

`dc_stubs.c:118` returns `0`. `0` is `OS_SOUND_MODE_MONO`, and
`src/audio.c:146` does:

```c
if (OSGetSoundMode() == 0) { Na_SetOutMode(1); }
```

so the game hard-locks itself to mono, and `ac_npc_p_sel.c:102` branches on the
same value. [traced] `kb/audio-plan-of-record.md` §9.1 specifies "22.05 kHz,
**STEREO locked**" for stage A. A stub returning a plausible constant has
quietly contradicted the plan of record.

### 15. A stub that is stale rather than missing

`dc_os.c:786`'s `__OSSetExceptionHandler` announces "install KOS
`irq_set_handler`… PLAN 3.2 depends on it". That work is **done**, elsewhere:
`dc_main.c:204` `pc_crash_protection_init()` registers `dc_crash_handler` for
`EXC_DATA_ADDRESS_READ/WRITE`, `EXC_ILLEGAL_INSTR` and `EXC_SLOT_ILLEGAL_INSTR`,
and the log shows `[DC/EXC] SH-4 exception recovery armed` at line 181.
[traced] The remaining stub is the GameCube-side entry point, which nothing in
AC needs; its note costs a false to-do entry every run.

---

## 4. In flight — do not pick these up

- **VMU / `CARD*`** (`dc/src/dc_card.c`) — save backend; `dc_vmu_write_file` and
  `CARDRename` are the two open stubs.
- **ARAM disc-backed LRU** (`dc/src/dc_aram.c`) — PLAN §3.1; see §3.3 above for
  the text blast radius.
- **Framebuffer probe** (`dc/src/dc_pvr.c`) — `kb/STATE.md` N2.
- **Asset working-set census** (`dc/src/dc_gx.c`) — `kb/STATE.md` N1, vertex half.
- **P7 size lever** — `kb/ram-plan.md`.

---

## 5. What I could not determine

- **What restoring the audio pump costs in `.text`.** I traced that it is
  disconnected; I did not measure what re-linking `Jac_VframeWork`,
  `Jac_UpdateDAC` and their callees adds. The RAM ledger has never budgeted it,
  and that number matters before anyone commits.
- **Whether `GXCopyTex` alone is sufficient** (§3.8). The record-vs-replay
  reasoning is inference; nobody has run a build with a submenu open.
- **What the player-select and town-creation scenes actually call.** I traced
  the title demo to `aAL_title_game_data_init_start_select`
  (`ac_animal_logo.c:143`) and confirmed the next scene is `SCENE_PLAYERSELECT`
  / `SCENE_START_DEMO` (`include/m_scene_table.h:46,50`). I did **not** trace
  their platform calls: they are dominated by asset loads the stub build cannot
  perform, so the trace is cheap the moment item 4 and the S4 loader exist and
  near-worthless before.
- **DVD stall magnitude on hardware.** `4,132,608 B ÷ 500 KB/s` is arithmetic.
  No burn has been timed.
- **Whether `Nas_smzAudioFrame`/`CreateAudioTask` being linked implies a second
  live audio path.** Both are at real addresses in the map; every chain I found
  to them runs through deleted code. Static reading could not close it. It does
  not change the conclusion — the queue is provably never drained.
- **The `dc_unimpl_dump()` roll-up has never printed.** It runs only from
  `dc_misc.c:345`'s `__OSUnhandledException` and from `main()`'s exit; the game
  never returns and has not faulted. Individual `[DC/TODO]` lines are the only
  signal today, and across **every** run in `~/.cache/oc-dc-harness/runs/`
  exactly three distinct stubs have ever been hit: `PADControlMotor`, `AIInit`,
  `ARStartDMA`. [traced]

---

## 6. What I would hand an agent tomorrow

**A. `DC_AUTOSTART=<frames>` in `dc_pad.c` (item 4).** ~30 lines, one env knob,
zero risk, and a *multiplier*: items 5–14 are unobservable until it exists, so
without it every one of them gets evaluated by reading code. This project's
history is a list of things that looked right statically and were not.

**B. Rate-limit `OSReport` at `dc_os.c:606` (item 2).** ~20 lines. 85 % of the
console is one line and the next agent to debug anything pays that tax. It is
also the only part of the audio problem that can be fixed without first
answering a CPU-budget question.

**C. `OSGetSoundMode()` → stereo (item 9).** One line, and it retires a stub
that is actively lying to the game against a written plan of record.

Explicitly **not**: the audio pump (blocked on the voice-count measurement,
which belongs on the PC build); EFB capture (unreachable until A lands and the
town draws); DVD read-ahead (invisible in the emulator, so unvalidatable today,
and it re-opens the single-threading assumption).

---

## 7. Index row for `CLAUDE.md` (not applied — paste into §3, "Live state")

```
| `kb/boot-blockers.md` | **what the running game hits next** — the stub audit, ranked by reach rather than difficulty, with in-flight work marked |
```
