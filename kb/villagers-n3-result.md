# N3 RAN — the villager wall is named (2026-08-13)

## ⭐⭐ THE ANSWER, after four runs: `mEv_CheckArbeit()` IS STUCK TRUE

**Read this first; §4 below is superseded.** The guest pass's OUTER gate was
split into its five terms and read:

```
arb[pass]: work=0 intro=0 demo1=0 demo2=0 hallo=0
gst: calls=10403 arb=10403 blkmax=0 exist=0(ea=0 jevt=0)
```

The five counters count PASSES and **short-circuit**, so `work` — the leftmost
term, `!mEv_CheckArbeit()` — is the wall. It is false on every one of 10,403
ticks. The other four are 0 because they were **never evaluated**, not because
they failed.

`mEv_CheckArbeit()` (`m_event.c:173`) is TRUE while any of
`mEv_SAVED_FIRSTJOB_PLR0` / `HRAWAIT` / `HRATALK` is set for this player.
Those are the flags Nook sets when he hands out the starting job and clears
when it is finished. **This port never finishes it, so they stay set forever
and `aSNMgr_chk_arbeit_and_demo_and_halloween()` blocks the villager loop by
design.** It is not a bug in the NPC manager at all.

### 🔴 THREE READINGS THIS DOCUMENT GOT WRONG, corrected

1. **`ea=0` is NOT an appear-type mismatch** (§4 below). The guest body is
   `if (!arb && !blkmax)`, so with `arb` TRUE every tick the per-villager loop
   **never executes** and `ea` counts code that never runs. §4's "two candidate
   causes" were both wrong because the premise was.
2. **`mEv_CheckFirstIntro()` is NOT the culprit**, despite `kb/RESUME.md` §8
   naming it. `intro=0` means it was never even evaluated — `work` fails first.
3. **`arb=` counts BLOCKED, not passed.** `dc_npcdiag.h` said so in writing
   ("Its two outer gates count BLOCKED, not passed") and the first analysis
   here read it as a pass anyway.

⭐ **And the kb's oldest claim about this — `kb/STATE.md` §A and
`kb/RESUME.md` §7.1's "nothing constructs a villager ACTOR" — is wrong.**
Construction is never *asked*: `mk: ent=10442 gate=10442 slot=0 called=0`.

### ✅ Also settled by the same run: the villagers config BOOTS

`MEMLEDGER FIT image_span=10705868 additive_heap=2576256 margin=3364020 OK`,
`ASSET MISSING 0`, deepest scene 18, with `DC_NPCTEX_POOL=1 DC_NPCMDL_POOL=1
DC_NPC_SEED=1` **and audio on** — i.e. the RAM profile of the villagers burn.

### The next step, and what it is not

`DC_NPC_ARBEIT_CLEAR=1` (`dc/src/dc_npcseed.c`) calls the game's own
`mEv_ClearPersonalEventFlag()` once at the seeder hook.
🔴 **It is a DIAGNOSTIC, not the fix** — it skips content the player is meant
to play. It answers exactly one question: is arbeit the LAST wall, or the first
visible one? ⚠️ It also clears `FIRSTINTRO`, so a positive result does **not**
isolate arbeit from intro; splitting them is the follow-up.

**The real fix is making Nook's starting job completable and persisted**, which
is the same save-path problem that keeps the town reseeding every boot.

---


**The diagnostic the kb has been asking to run since 2026-08-10 has now run.**
`DC_NPCDIAG=1 DC_NPC_SEED=1 DC_NPCTEX_POOL=1 DC_NPCMDL_POOL=1`, plus
`DC_AUTOSTART=300` and `-DDC_AUTOWALK=1`, 900 s in Flycast, 12,087 logic ticks.

```
[DC/NPCDIAG] w=80 t=12087
  ct:    mgr=1 ctl=5 clip=5 cloth=10/10
  wade:  none=12048 start=1 prog=37 end=1 err=0 mode=2
  reg:   calls=2     exist=28  scope=0 appear=0 utnum=0 make=0
  ut:    calls=0 col=0 fgcol=0 hgap=0
  gst:   calls=12048 arb=12048 blkmax=0 exist=0 scope=0 appear=0 utnum=0 make=0
  mk:    ent=12087 gate=12087 slot=0 idx=0 called=0 ret=0
  setup: ent=8 chk=8 actor=8
  oob=0
[DC/NPCSEED] pre ids=6 homed=6 | seeded id=8 home=8 | want=14 max=14
```

---

## 1. 🔴 THE KB'S DIAGNOSIS IS REFINED: IT IS NOT "ACTOR CONSTRUCTION"

`kb/STATE.md` §A and `kb/RESUME.md` §7.1 both say *"nothing constructs a
villager ACTOR"* and point downstream of `npclist`. **The construction path is
never asked to run.** `mk: ent=12087 gate=12087 slot=0 … called=0` —
`aSNMgr_make_npc` runs every tick, passes its `CLIP && setupActor_proc` gate
every tick, and finds `make[]` **empty** every tick. Nothing downstream is
broken; it has no work.

⭐ **And 8 actors DO get built** (`setup: ent=8 chk=8 actor=8`) via a path that
is not the villager spawn — `mk: called=0` proves none came from `make_npc`.

## 2. Three of the five hypotheses are DEAD

| # | hypothesis | verdict |
|---|---|---|
| 2 | all ten cloth banks failed to reserve | 🔴 **DEAD** — `cloth=10/10` |
| 3 | `CLIP(npc_clip)` is NULL | 🔴 **DEAD** — `clip=5` |
| 4 | `aSNMgr_get_safe_utnum` rejects every unit | 🔴 **DEAD** — `ut: calls=0`, never reached |

## 3. ✅ Hypothesis 1 CONFIRMED — the GUEST proc is sticky

`reg: calls=2` against `gst: calls=12048`. The REGULAR pass ran **twice in
12,087 ticks**; `mode=2` (GUEST) the rest of the time.

⚠️ **With one refinement the decision table got wrong.** It predicted
`wade: start=0`. Observed `start=1 prog=37 end=1` — a wade transition *does*
fire, so `mFI_GetPlayerWade()` is not pinned at `WADE_NONE`, and REGULAR is
starved rather than unreachable. `aSNMgr_set_npc_regular` tails into
`aSNMgr_setup_set_proc(manager, GUEST)` on `mFI_WADE_NONE`
(`ac_set_npc_manager.c:1189-1196`) and only `case mFI_WADE_START:` (`:1256`)
restores REGULAR, so one transition buys exactly one REGULAR pass.

## 4. ⭐⭐ THE ACTUAL WALL: the GUEST path's per-villager gate NEVER passes

```
reg: exist=28    <- aSNMgr_chk_exist_and_appear            passes (14 x 2 calls)
gst: exist=0     <- aSNMgr_chk_exist_and_appear_and_event  NEVER passes, 12,048 calls
```

The two differ by exactly one term (`ac_set_npc_manager.c`):

```c
static int aSNMgr_chk_exist_and_appear_and_event(...) {
    if (aSNMgr_chk_exist_and_appear(manager, appear_type, idx) &&
        ((manager->npc_info.joint_event >> idx) & 1) == 0) {   /* <-- the delta */
        ret = TRUE;
    }
}
```

and the GUEST caller (`:1216`) asks for `mNpcW_APPEAR_STATUS_REGULAR`.

🔴 **TWO CANDIDATE CAUSES, AND THE CURRENT COUNTERS CANNOT SEPARATE THEM.**
Do not guess — split `exist` into two counters and re-run:

1. **`npc_info.joint_event` has the villagers' bits set**, so the `& 1) == 0`
   term is false for every index.
2. **The `appear_type` argument mismatches.** GUEST passes
   `mNpcW_APPEAR_STATUS_REGULAR`; if the seeded villagers' `winfo` appear
   status is something else, `chk_exist_and_appear` itself fails inside the
   `_and_event` wrapper for a reason that has nothing to do with events.

**The next N3 change is one line of instrumentation**, not a fix: count the
inner `chk_exist_and_appear` result and the `joint_event` bit separately at
`:1216`. That is decisive between (1) and (2).

## 5. ⚠️ `reg: scope=0` is probably BENIGN — do not chase it first

28 villagers passed `exist` in the REGULAR pass and all 28 failed
`aSNMgr_check_in_scope`, which is a plain rectangle test of the villager's
`position` against a box built around the player
(`aSNMgr_renewal_set_scope`, `:224`; `aSNMgr_check_in`, `:246`).

`npclist->position` is real — it is derived from `home_info` at
`m_npc.c:2818-2819`, and `[DC/NPCSEED]` confirms all 14 have valid homes. So
`scope=0` over **two** samples is exactly what you would see if the player
simply was not near a house at either moment. **It is hypothesis 5 (benign)
until a run with more REGULAR passes says otherwise** — which means fixing §3
first, then re-reading `scope`.

## 6. What to do, in order

1. **Split the `gst: exist` counter** (§4). One line, one run, decisive.
2. **Then fix whichever of the two it names.**
3. **Only then** re-read `reg: scope=`, which needs §3's stickiness addressed
   to produce enough samples to mean anything.

⚠️ **This was a Flycast run and that is fine** — the villager path is game
logic, not timing, so the emulator is a valid instrument here. Nothing about
this needs a burn.
