# RAM levers — the ranked ledger

## ⭐⭐ 2026-08-06 (session 6) — RAM IS NO LONGER THE BINDING CONSTRAINT

**Read this before costing anything else in this file.** The image after today's
content spend (committed `296a1d2`), measured from three links:

| | shipping | + interiors/winter | **+ gyroids (now)** |
|---|---:|---:|---:|
| `.text` | 2,753,700 | 2,793,284 | **2,854,108** |
| `.bss` | 3,945,484 | 4,428,076 | **4,791,884** |
| image span | 8,926,124 | 9,446,380 | **9,878,540** |
| `margin` | 6,061,268 | 5,541,012 | **5,109,364** |

Real headroom — `margin` **minus** the 3,056,276 B libc peak, which is the only
honest form (rule 6) — went from **~146 KB on 2026-08-04 to ~2.05 MB**, and that
is *after* spending 952,416 B on content. `MEMLEDGER OK`, `ASSET MISSING 0`,
`aram LOST 0`, `deepest_scene 18`, `run_report --vs` clean, town `us/v`
3.07 → 3.09, gyroids confirmed rendering by a human.

⚠️ **THE ~2.05 MB IS STALE — IT IS ~649 KB NOW (re-derived 2026-08-09).** Two
deliberate spends came after this table and nobody re-ran the arithmetic:
`DC_ARAM_WINDOW` 131072 → 1048576 (+917,504 B of additive heap, and it cut disc
reads 40×) and `DC_AUDIO=1` disabling the S8 jaudio `.bss` shrink (+455,848 B of
span). The current inequality is in `kb/STATE.md`. ⚠️ **The 3,056,276 B libc
peak itself was measured on 2026-08-04 at the OPENING keep list and has never
been re-derived** — the honest next step is an OOM pair on the current config,
not more arithmetic. The conclusion below is unchanged: fit is not what binds.

**The consequence for this file: "does it fit" has stopped being the question.**
What binds now is **residency** — 8,813,054 B of asset destination arrays can
never all be resident, so the keep list still decides what exists. A lever is
now worth ranking by *what content it delivers per byte*, not by how much of a
deficit it closes. The deficit is gone; `kb/closed.md` records the boot that
proved the other extreme (a full non-stub image) is unreachable in principle.

⚠️ **Cost a keep-list addition from two links.** The gyroid set was estimated at
155,360 B by summing its `Vtx` arrays and came in at 432,160 B of span — 2.8×
low, because the files also carry textures and display lists. (`kb/traps.md`.)

## ⭐ L0 — OPTIMIZATION, APPLIED 2026-08-06. −2,826,288 B of `.text`

**Bigger than every other lever in this file put together, and it was banned
until this session.** `src/` at `-Os` (`DC_OPT_PROFILE=perf`, the default):

```
.text  5,506,964 -> 2,680,676     (-2,826,288)
.data  2,337,980 -> 2,224,832     (-113,148)
.bss   3,945,356 -> 3,945,484     (+128, noise)
```

The `-O3` hot list then spends **+48,476 B** of that back for 3.5 ms of
frame time. ⚠️ **That figure was measured with 14 entries on the list; it holds
18 today** — the four jaudio TUs joined the same day to fix a measured stutter,
and their `.text` cost was never broken out. `DC_OPT_PROFILE=size` gives it up again if an image will not fit;
`DC_OPT_PROFILE=o0` is the byte-identical revert.

⚠️ **EVERY NUMBER BELOW THIS LINE WAS COSTED AGAINST A 5.5 MB `.text` IMAGE.**
The fit inequality, the `margin=` readings, and every "does X fit" verdict in
this file predate a 2.83 MB image shrink. Re-measure
before spending it — and read rule 6 first (`margin=` is not headroom).

Evidence: `kb/state-log.md` 2026-08-06. Post-mortem on the ban: `kb/closed.md`.


Every way found so far to close the RAM gap, with status. **Read this before
planning any size work.** Read `kb/closed.md` before *proposing* any — several
obvious ideas are already dead.

⚠️ **THERE IS NO GAP ANY MORE.** This file used to open with one ("6,999,924 B
over", then "4,705,628 post-ARAM-pager"); `-Os` closed it on 2026-08-06 and the
image has fitted ever since. Every entry below that is *ranked* against a
deficit is ranked against a number that no longer exists — read them as "what
this delivers per byte", per the section above. `kb/STATE.md` carries the
current inequality; this file is the ledger, that file is the plan.

## ⭐ 2026-08-04 — TWO LEVERS APPLIED, and they bought the acre fix

Both are measurements the project had never taken, and together they are
~1.15 MB. They are what made `tools/dcstub/keeplist-town.txt` affordable.

| lever | bytes | status |
|---|---|---|
| **The arena cut.** `DC_ARENA_BYTES` 1,900,000 → **1,200,000** | **+700,000** to libc | **APPLIED.** Licensed by the first TOWN reading of `DC_ARENA_PROBE`: `[DC/ARENA] zelda used=289536 free=1124944 largest_free=1124944`. Every previous reading was the title screen, which is why no doc would license a cut. Leaves ~425 KB above measured use |
| **jaudio `.bss` shrink**, `make_src_shrink.py` S8/S9, keyed to `DC_AUDIO=0` | **−450,368** | **APPLIED.** `seq` 256→8 tracks (−267,840), `CHANNEL` 256→8 (−79,360), `dvd_buf` (−49,152), `CALLSTACK` 128→16 frames with its modulus (−28,672), `pc_task_buf` (−25,344). Justified by the pump being `#if DC_AUDIO` and every downstream function having exactly one caller. Each rewritten TU carries an `#error` against a stale tree compiled with `DC_AUDIO=1` |

⚠️ **The S9a `dmabuffer` rule is worth ZERO, not the −61,440 its design
claimed.** `dc/build/gcdrop.txt` shows `--gc-sections` already removes
`.bss.dmabuffer`, and `sh-elf-nm` finds no `_dmabuffer` in the linked ELF. The
rule ships (a `--no-gc-sections` debug link puts it back) but is excluded from
`EXPECTED`. Quoting it would be exactly the L3 class of error this file exists
to prevent.

⚠️ **`DC_AUDIO=1` gives all 401,216 B of S8 back** — the pools are only dead
while the sequencer is. A sound build costs ~455,848 B more span than a silent
one; that is why sound is off by default and `DC_AUDIO_SCENES` is opt-in.

**The number to plan against is NOT `margin=`.** See `kb/traps.md`,
"`MEMLEDGER FIT … OK` does not mean the image boots".

Two results that reorder this list, both derived in `kb/STATE.md`:

- **L4 (`.text` overlays) is NOT needed.** The gap closes without touching
  `.text`. Earlier docs called it "the fork in the road"; that was wrong.
- **L1's pool is the binding constraint.** L1 lands `.bss` at 3,644,150, but
  the pool it loads into is additive heap and may be at most ~498,250 B unless
  L3 also lands. L1 alone is not really sufficient.

⚠️ **[VOID 2026-08-06 — kept only so the shape of the error stays visible.]**
~~Only **layout** levers are legal. `-O0` is a user directive, so anything that
changes instruction selection is banned:~~ **Codegen is a first-class lever on
both axes now (L0 above): `-Os` was worth 2,826,288 B of `.text`, more than
every layout lever in this file combined.** The table's first row is reversed;
every other row is still accurate.

| Lever | Changes instruction selection? | Allowed |
|---|---|---|
| ~~`-O1/-O2/-Os`, LTO, `-mrelax`~~ | yes | ~~**no**~~ → **`-Os`/`-O3` per `DC_OPT_PROFILE`; LTO and `-mrelax` untested** |
| `.bss` right-sizing, arena sizing | no | yes |
| Moving data/code to `/cd`, demand loading | no | yes |
| Linker script placement, code overlays | no | yes |
| Offline asset conversion / decimation | no | yes |

---

## ⚠️ 2026-08-05 — THE RULE THAT INVALIDATES HALF THE POOL ARITHMETIC IN THIS FILE

**In a stubbed image, an asset class's resident cost is what the KEEP LIST
kept, not what the class totals.** `DC_ASSET_STUB` already dropped the rest: an
unkept asset is a 1-byte `.bss` symbol and its load is suppressed. Every "pool
X and free N bytes" claim written before this date costed N against the
**non-stub** total, i.e. against bytes that have not been resident since S1.

Consequence, and it inverts the purpose of a pool: **a pool converts MISSING
into PRESENT at a bounded resident cost.** It usually does not save anything.
Cost one against **the alternative — keeping the class — never against the
class total.** Measured, on the two pools that landed:

| | non-stub total | resident before | pool `.bss` | net | delivers |
|---|---:|---:|---:|---:|---|
| R2 villager textures | 1,154,944 | **90,464** | 78,872 | ~−4,700 | 21 species → 236 |
| R3 villager models | 438,640 | **5,536** | 120,956 | **+115,424** | 1 species → 32 |

R3 is justified only because keeping the same 32 species costs 194,400 B, so
the pool is 73,568 B cheaper than the content it delivers.

**The exception is the acres**, and it is the only one: all 242 summer acre TUs
really are kept, so **815,024 B of `grd_s_*` `*_v` vertex arrays is genuinely
resident** (measured off `dc/build/AnimalCrossing.map`, whose `.bss` sums
exactly to the ELF's 4,059,052 B section). An acre pool would free real RAM.
Full derivation: `kb/state-log.md`, 2026-08-05.

---

## Applied

### A5. R2 + R3 — the villager pools. **Written, DEFAULTED OFF, +110,724 B `.bss` if turned on** (2026-08-05)

`dc/src/dc_npctex.c` (236 villager texture sets out of 16 × 4,832 B slots) and
`dc/src/dc_npcmdl.c` (32 villager model species out of 16 × 7,552 B slots).
**Both default to 0**; `DC_NPCTEX_POOL=1` / `DC_NPCMDL_POOL=1` arm them. Listed
here because the code is applied, **not because they are savings** — see the
rule above.

⚠️ **They are off because the port has no villagers to load into them, and that
is measured.** `mNpc_SetNpcList` fills the town from the save's `Animal_c
animals[]` (`m_start_data_init.c:559`); the VMU path is unwired, so
`[PC] No save file found` and **not one villager actor is ever constructed**.
Two 900 s runs reaching scene 9 and walking printed zero `[DC/NPCTEX]` /
`[DC/NPCMDL]` lines, because their only entry point
`aNPC_dma_draw_data_proc` (`ac_npc_ctrl.c_inc:687`) never runs. **Wiring the
save (PLAN N2b) is a prerequisite for testing either pool, not a nice-to-have.**
Nothing in the byte columns below has been observed under load.

```
R2   keep list removed  -90,464 | pool +78,872 | .rodata +~6,900 | net  ~-4,700
R3   keep list removed   -5,536 | pool +120,956 | .rodata +~4,600 | net +115,424
```

Two mechanisms worth reusing, both the same trick R1 (A4) proved: the stub
rewriter leaves each unkept asset as a **1-byte `.bss` symbol with a unique
address**, so the pointer already sitting in a table is a unique *key* naming
which asset the slot wants. R2 needs nothing more — villager textures reach the
RDP through N64 segment registers bound per draw (`ac_npc_draw.c_inc:269-278`),
so a load is 16 pointer writes into `npc_draw_data_tbl[]` (writable `.data`).
R3 does, because a `Vtx` array is named by a linker-resolved `R_SH_DIR32` word
baked into an initialised `Gfx`, and `emu64::seg2k0` returns any `0x8Cxxxxxx`
address unchanged — so moving the array means moving 933 words across the 32
species.

⚠️ **The seam is NOT `--wrap`.** `--wrap=mNpc_SetNpcList` matches nothing on
sh-elf and is not diagnosed — see `kb/traps.md`.

⚠️ **16 fixed max-sized slots waste 15,248 B** against the 16 largest species
packed end to end (105,584 B). A bump arena recovers it at the cost of a second
failure axis; `DC_NPCMDL_SLOTS` / `DC_NPCTEX_SLOTS` are the knobs to cut first.

### A4. R1 — acre ground textures demand-loaded. **−81,856 B `.bss`** (2026-08-05)

**The first asset pool this port has ever shipped, and it is small on purpose:
it proves the seam.**

96 `mFM_grd_*` symbols — **150,880 B** (46 summer / 73,312 B, 41 winter /
70,144 B, 9 shared / 7,424 B) — were pure `bcopy` **sources** into 32
always-resident staging buffers in `src/game/m_bg_tex.c` (33,792 B, the
`*_dummy` row L8 flagged as "placeholders, unverified"), filled by
`mFM_LoadBGCommonTex` (`src/game/m_field_make.c:1101-1133`). **Zero other
consumers anywhere in `src/`, `include/` or `pc/`**, so the sources never have
to be resident — only the 32 destinations.

| | before | after | Δ |
|---|---:|---:|---:|
| `.bss` | 4,027,212 | 3,945,356 | **−81,856** |
| `MEMLEDGER margin` | 3,103,956 | 3,191,348 | **+87,392** |

`ASSET MISSING` 0, `aram LOST` 0, no OOM, `deepest_scene` 18 unchanged,
`fps_p50` 24.1 → 24.2, and **screenshot-verified on a `DC_BGTEX_DEMAND=0` vs
`=1` pair** — measurement rule 2 satisfied, not just the counters.

**The seam is NOT `--wrap`, and that is the reusable part.**
`mFM_LoadBGCommonTex` and all six segment tables are `static`, and `bcopy` has
no symbol in the linked image at all (GCC folds it to `memmove`). The lever is
`tools/dcstub/make_src_shrink.py` rewriting the one `bcopy` call in the
shrink-tree twin of `m_field_make.c`; `src/` is untouched.

**What makes it cheap enough to copy:** the stub rewriter leaves each unkept
source as a **1-byte `.bss` symbol with a unique address**, and the segment
tables still reference those symbols by name — so `bg_tex_tbl[i]` is a unique
*key* identifying which asset the slot wants. `dc/src/dc_bgtex.c` looks the
pointer up in a generated 96-row map and calls the existing
`dc_stub_keep_load_one()`; a miss falls back to `memmove`, so a kept asset still
works. **No season logic and no `tex_idx` logic is duplicated in `dc/`.** Kill
switch `DC_BGTEX_DEMAND=0`.

Two things that came with it, both in `kb/traps.md`: vanilla over-reads
`mFM_grd_s_beach_tex` by 1,024 B and the loader must reproduce that; and the
27 scattered seeks this introduces are **0.5-2.7 s [UNMEASURED]** until a
sorted batch helper mirrors `dc_keep_sweep()`.

Also defuses half a dated time bomb — the 41 winter ground textures were never
in the keep list and are now loadable. ⚠️ The 84 `obj_w_*` structures are still
absent, so the winter bomb is **reduced, not cleared** (`kb/RESUME.md` §4).

### A3. S3 pass one — −1,746,528 B of `.bss` (4th session, commit `b0e009d`)

Measured against a clean full rebuild, not estimated: `.bss` 12,415,796 →
10,669,268, `.text` +676, span −1,810,816.

- **`tools/dcstub/make_src_shrink.py`, −1,159,392 B.** Seven numeric literals
  rewritten into `dc/build/shrinksrc`, swapped in by `dc/Makefile`.
  `DC_SRC_SHRINK=0` is a byte-identical revert. `src/` untouched.
  - **actor overlay arenas, −422,192.** `aSTR_overlay` 294,912→512 ·
    `aNPC_{n,s,k,e}` 75,840→192 · `aGYO` 30,720→32 · `aINS` 21,528→72. They are
    **dead**, not "mutually exclusive": nothing reads them, and the decomp says
    so itself at `include/m_actor_dlftbls.h:16` and
    `src/actor/npc/ac_npc_ctrl.c_inc:549`. Shrink the *element*, not the count,
    so every loop bound, index and NULL check stays byte-identical.
  - **`prbuf`, −614,368.** Its only writer is a stubbed no-op — `GXCopyTex`
    (`dc_gx.c:1524`) and `GXBeginDisplayList` (`:1613`) do nothing on DC.
  - **`audiomemory` `0x90000`→`0x76000`, −106,496.** Measured allocation
    ceiling is 481,152 B; 2,176 B margin kept. **Do not cut further** —
    `misc_heap` runs at 99.4% and `Nas_WaveDmaNew` (`system.c:431`) silently
    `break`s on exhaustion, dropping voices with no error.
  - **`graph`/`padmgr`/`irqmgr` stacks, −16,336.** `dc_stubs.c:96`
    `OSCreateThread` returns 0; no thread ever starts.
- **`dc_gx` + `dc_os`, −278,796 B.** Vertex staging 8192×40 B → 2040×32 B.
  `color1`/`_pad`/`_reserved` are never written by anything in the tree. 2040
  not 2048: the split point must be a multiple of 12 and `2048 % 3 = 2` would
  cut a triangle in half. **Overflow now flushes and continues** instead of
  dropping geometry — that is what made the old 8192 a correctness requirement
  rather than a choice. `dc_os.c` hand-rolls the `ocbp` loop so KOS's 16,384 B
  `arch_dcache_purge_all` buffer is never instantiated.
- **`pc_m_card`, −308,234 B.** `l_keepMail`/`Original`/`Diary` were a pure
  double buffer of `l_aram_block_p_table[]`, deleted; the GCI read now
  validates all three checksums before committing any byte, with rollback
  through `mCD_set_aram_save_data()`. `l_keepSave` retyped `Save`→`Save_t` and
  moved to `zelda_malloc` from the arena bucket 6 already reserves, so additive
  heap is unchanged. Also fixed a latent overrun: the 32-byte-aligned size
  table is *larger* than the structs, so the old `memcpy` wrote 28 bytes past
  `l_keepMail`, landing in linker padding by luck.

**Two claims REFUTED during implementation and deliberately not shipped**
(−58,368 B not taken): jaudio's `CALLSTACK` is a live task-frame pool —
`GetCallStack()` (`dvdthread.c:45`) hands out 128 slots and
`pc_dvd_process_all_tasks()` drains them — and `pc_task_buf` is the live audio
command buffer `RspStart2` consumes every frame (`neosthread.c:35-39`).
Shrinking either corrupts audio silently.

### A1. Bucket 6 dead weight — −1,294,496 B of additive heap (3rd session)

Real memory no longer `memalign`ed at boot, not a ledger edit.

- **XFB double buffer, −1,228,800 B.** `JUTXfb::initiate`
  (`src/static/JSystem/JUtility/JUTXfb.cpp`) has a `#if defined(TARGET_DC)`
  branch leaving all three `mBuffer[]` **NULL**. Every consumer terminates at
  `VISetNextFrameBuffer` (`dc_vi.c`, a no-op), `GXCopyDisp` (`dc_gx.c`, ignores
  `dest`), or `JUTChangeFrameBuffer` — the PVR owns the real framebuffer and it
  lives in VRAM.

  NULL is safe *because the code already handles it by construction*:
  SingleBuffer mode leaves `mBuffer[1]/[2]` NULL, `getDrawnXfb()` already
  returns `nullptr` on a negative index, and `JUTDirectPrint`'s own constructor
  calls `changeFrameBuffer(nullptr, 0, 0)`. The buffer *indices*, which drive
  `JFWDisplay`'s rotation, are untouched.

  ⚠️ The old budget research advised handing back a **non-NULL dummy**. That advice was deliberately **not** followed: a small dummy leaves
  `JUTDirectPrint` "enabled" and aimed at 32 bytes, which is a heap-corruption
  trap. NULL disables it, which is correct on DC.
- **GX FIFO, −65,696 B.** `jsyswrap.cpp`'s
  `JC_JFWSystem_setFifoBufSize(0x10001)` → `0x100` under `TARGET_DC`. A token
  allocation is kept so `JUTGraphFifo`'s ctor, its `GXFifoObj` and the
  `~JUTGraphFifo` free path stay structurally identical.
- **`DC_MAIN_MEMORY_SIZE` 4,000,000 → 2,705,504** (`dc_platform.h`, and
  `DC_BUDGET_JKRHEAP` to match — `dc_os.c` static-asserts equality). Cut by
  exactly what the two dead allocations consumed, so **`__osMalloc`'s usable
  pool is unchanged at ~2.6 MB.**

Verified by re-link: `.text` 6,318,568 → 6,318,552, `.data`/`.bss` unchanged
(these were heap bytes). Image span moved +96 B on alignment, net −1,294,400 B.
**Not verified at runtime** — the image still does not boot on size alone, so
the new `MEMLEDGER FIT` line has never printed.

### A2. `.bss` right-sizing — −1,111,040 B (2nd session)

Measured delta equals the sum exactly: `prbuf` `sizeof(u32)`→`u16` −614,400 ·
`TEX_BUFFER_DATA_SIZE` `0x80000`→`0xC000` −475,136 · `TEX_BUFFER_BSS_SIZE`
`0x4000`→`0x400` −15,360 · `TEXTURE_CACHE_LIST_SIZE` 1024→256 −6,144. All four
revert PC-port inflation to **retail GameCube values**, so sufficiency is
proven by the shipped product.

---

## L1. Asset destination arrays — 8,771,358 B. THE lever. Not implemented.

64.5% of `.bss`, and the only single item that covers most of the gap.
Confirmed to the byte by *three* independent methods (`mem-budget.md` §2 symbol
attribution, the asset agent's loader replay, the build agent's `nm -S` sweep).

These are `#ifdef TARGET_PC` placeholder arrays that `pc_assets.c` fills
eagerly at boot. Fix is demand-loading into pooled storage: a **loader-only
change, no codegen.** Everything else is a rounding error next to this.

**Two dead ends already closed, do not re-walk them:**

- ⚠️ An earlier session claimed the 8.5 MB was "free PC scaffolding" that would
  vanish by reverting to the GameCube path. `research-budget-premises.md` §6.2
  says that is **false as a RAM lever**: under the non-`TARGET_PC` branch those
  arrays become *initialised* data — the same resident bytes moved from `.bss`
  to `.data`, plus disc bytes. On a no-MMU sbrk machine that is neutral at
  best. **The saving comes from demand-loading, not from flipping the define.**
- ✅ **SETTLED:** `find src -name '*.inc'` returns exactly **three** files
  repo-wide (`src/game/m_huusui_room_ovl_data.inc`,
  `src/actor/npc/ac_npc_rtc_think.c.inc`, `…_talk.c.inc`) and `find src/data
  -type d -name assets` returns **nothing**. There is no
  `src/data/**/assets/*.inc` tree, so the non-`TARGET_PC` branch **cannot build
  at all**. "Revert to the GameCube path" was never an available option. This
  is the answer to `research-budget-premises.md` §6.2 question 2a — do not
  spend another pass on it.

## L2. Resident REL blob — 16.56 MB peak. SOLVED, tool built and verified.

`dcasset pack` emits `assets.pak` (8,917,568 B) + a 51,104 B resident index,
replacing the resident `foresta.rel` + `main.dol` (16,558,776 B). Round trip
replays 16,365 references over 8,884,894 B with **zero mismatches**. Chunks are
pre-byte-swapped offline (SH-4 never runs `do_swap`) and laid out in real load
order — 82 backward reads, max reach 7,520 B, so an **8 KB window gives zero
seeks**; one linear 8.9 MB read, 17.8 s at 500 KB/s. Also replaces
`foresta.rel` on disc (−6.7 MB, no Yaz0 at boot).

**Remaining work is the runtime loader in `pc_assets.c`** — and that same
loader is what unlocks L1. See `kb/asset-pack.md`.

Two rules from the pack author:

1. **Log window faults, never swallow them.** A regenerated `pc_assets.c` that
   reorders calls silently degrades to `fs_seek` + binary search — correct, but
   minutes slower.
2. **Do not delete `do_swap`.** A future regeneration with a swap conflict
   ships that chunk raw with the `PRESWAPPED` bit clear.

## L3. The ranked remainder — RE-COSTED 2026-08-01. 2,928,267 B, not 4.3 MB.
<!-- and re-costed again 2026-08-02: the s_assets row came in 223,145 B under
     its estimate and the data_bgd row 9,520 B over, so the true remainder is
     2,714,642 B. See Corrections 0 and 1. -->


Six agents re-derived every row against the real ELF. **Every estimate in the
old `research-size-*` set was wrong, most of them by a lot, and two of the
stated *mechanisms* were impossible** — which is why those files were deleted
(`kb/closed.md`). The originals are kept in the right
column so nobody re-proposes them.

| item | defensible | claimed | status |
|---|---:|---:|---|
| `s_assets[]` name strings | ~~−821,569~~ → **−598,424** | −0.89 MB | ✅ **BANKED 2026-08-02** as `make_src_shrink.py` rule **S6**. Deletion, as predicted — but the byte figure was **overstated by 223,145 B**, see below |
| actor overlay arenas | −422,192 | −0.46 MB | ✅ **BANKED** (A3). Dead, not "mutually exclusive" |
| `pc_m_card` | −308,234 | −0.28 MB | ✅ **BANKED** (A3) |
| `dc_gx` | −262,400 | −0.24 MB | ✅ **BANKED** (A3), over estimate |
| `data_bgd` collision split (the `.data src/data` row's S3-eligible part) | ~~−236,544~~ → **−246,064** | −1.94 MB | ✅ **BANKED 2026-08-02** as `make_src_shrink.py` rule **S7**. Came in **9,520 B OVER** its estimate — the only row so far that did. See "Correction 1" below. The rest of the `.data` row is S4 work |
| `audiomemory` | −106,496 | −0.65 MB | ✅ **BANKED** (A3). **AICA is impossible here**; the lever is "shrink" |
| census: `prbuf` | −614,368 | — | ✅ **BANKED** (A3) |
| census: `sys_stacks` + KOS `buffer.4` | −32,720 | — | ✅ **BANKED** (A3) |
| census: jaudio `CALLSTACK`/`pc_task_buf` | ~~−58,368~~ | — | ❌ **REFUTED** — both are live. See A3 |
| **banked so far** | **−2,591,016** | | measured, clean rebuilds (1,746,528 + S6's 598,424 + S7's 246,064) |
| **still live in S3** | **0 — S3 IS DONE** | | |
| `.data` display lists → S4 pool | −901,300 | | **belongs inside S4, not S3** |

### Correction 0 — the `s_assets[]` figure was 37% too big [MEASURED 2026-08-02]

`−821,569` was derived from `pc_assets.c`'s **total** `.rodata` contribution
(888,853 B). That total is the name-string
pool **plus the 347,880 B `s_assets[]` table itself** — and the table is live:
its `dest`/`size`/`rom_off`/`rom_src`/`swap` fields are the entire asset load.
Only the strings and the `const char* path` slot that points at them can go.

Measured across two clean full rebuilds of the whole tree that differ only in
whether the rule ran:

| | before | after | Δ |
|---|---:|---:|---:|
| `.rodata` | 1,057,364 | 458,716 | **−598,648** |
| `.text` | 5,283,456 | 5,283,680 | +224 (the replacement loader) |
| `.data` / `.bss` | — | — | 0 |
| image (`sh-elf-size` dec) | 19,824,552 | 19,226,128 | **−598,424** |
| image span | `0x12e81c0` | `0x1255f60` | **−598,112** |

`"^assets/"` strings in the ELF: 16,365 → 1,870, i.e. exactly the 14,495 table
rows. Two follow-ons this exposes:

- The **1,870 survivors** are the per-TU `_pc_load_src_*` call sites in `src/`,
  a separate ~75 KB pool needing a different (769-TU) mechanism. Small, and it
  dies anyway when S4 replaces those loaders.
- `s_assets[]` is now **289,900 B of pure `.rodata` table**. It is a disc-index
  candidate in S4 (`kb/asset-pack.md` already carries the same five fields per
  chunk), but only after the loader exists — not an S3 item.

### Correction 1 — the `data_bgd` split beat its estimate [MEASURED 2026-08-02]

`−236,544` had no derivation anywhere in `kb/`; it was an orphan number, and
"collision" in its name means the **collision map**, not a symbol collision.
`data_bgd` is singly defined — it is not part of the 1,367-symbol
`--allow-multiple-definition` family. It is also **`.data`, not `.bss`**.

The table is 295 acres × `sizeof(mFM_bg_data_c)` = 1076 B = **317,420 B**, and
302,080 B of that (95.2 %) is `mCoBG_Collision_u collision[16][16]`. That member
has exactly **one** reader in the whole tree — `m_field_make.c:271` hands
`bg_data->collision[0]` to `mFM_BgUtDataSet`, which walks 256 units strictly
forward into the heap-resident `mFM_bg_info_c::collision` once per block load.
Nothing indexes it randomly, memcpy()s it, or byte-swaps it. So it does not have
to be an array; it only has to be replayable in order. S7 run-length-codes it and
expands it at that call site.

Measured across two clean full rebuilds of the whole tree differing only in whether
the rule ran:

| | before | after | Δ |
|---|---:|---:|---:|
| `.data` | 2,638,872 | 2,337,976 | **−300,896** (`data_bgd` 317,420 → 16,520) |
| `.text` col (carries `.rodata`) | 5,749,148 | 5,803,980 | **+54,832** (palette 1,520 + stream 53,150 + decoder 162) |
| `.bss` | 11,145,696 | 11,145,696 | **0** |
| image (`sh-elf-size` dec) | 19,533,716 | 19,287,652 | **−246,064** |
| image span | `0x12a12e0` | `0x1265100` | **−246,240** |

**Verified end to end, not asserted:** the palette and stream were read back out
of the *linked* ELF, decoded with the same algorithm the C decoder runs, and
compared against the 295 × 1,024 B of `data_bgd[].collision` from the previous
build — **bit for bit identical, all 295 maps**. `_graph_proc` resolves once;
`data_bgd` is absent from the ELF with no dangling `U` reference, which also
proves it had exactly one definition; each new symbol has exactly one defining
input section in the map.

Two things worth keeping:

- **The palette is emitted as C initialiser *text*, not as packed `u32`.** The
  compiler does the bitfield packing, so the generator never has to know the bit
  layout of `mCoBG_CollisionData_c` and cannot get it wrong. Cross-checked
  first: the 380 distinct unit texts map one-to-one onto 380 distinct `u32`
  values in the old ELF.
- **No header is shadowed.** `include/m_field_make.h` reaches 61 TUs *and ten
  other headers inside `include/`*, so a shadow could never reach all of them —
  the S1a half-apply hazard. Both TUs that name the type get a local twin plus
  `#define mFM_bg_data_c …` / `#define data_bgd …` placed **after** the includes.

Also measured, and rejected as too weak: plain dedup of identical collision maps
is worth only **38,912 B** (295 arrays, 257 distinct). The saving is in the
run-length structure, not in duplicate acres.

### Three structural corrections — these change the plan, not just the numbers

1. **"Six measured, mutually independent moves" is FALSE.** The largest tranche
   of the `.data` row — 1,014,088 B of `Gfx` display-list bodies — is strictly
   **downstream of S4**: its relocation targets are pool addresses that do not
   exist until S4 assigns them. The `data_bgd` collision split is the part that
   could be banked in S3 and it has been (−246,064, rule S7, Correction 1);
   the rest is scheduled inside S4.
2. **L6 and L3 are mutually exclusive, not additive.** L6's 95,774 B of
   aliasable `.data` is entirely `cKF_*` keyframe tables inside `src/data/**`
   — exactly what L3's `.data` row moves to disc. If L3 lands, L6 is worth 0.
3. **L1 is undercounted by 159,037 B.** `stub.list` misses 12 files whose
   arrays are `pc_load_asset` destinations but whose bounds are macros, or
   which are `.c_inc` under `include/`. Biggest:
   `src/actor/npc/ac_npc_needlework_gba.c_inc` 84,704 ·
   `src/static/nintendo_hi_0.c` 39,168 ·
   `src/static/JSystem/JFramework/JFWSystem.cpp` 16,765.

### Two mechanism findings worth more than their bytes

- **Branch trampolines.** Turning `Gfx foo_model[]` into `Gfx *foo_model` would
  change the symbol's *type* and require rewriting **1,325 `extern Gfx x[];`
  sites** in hand-written decomp, silent on failure. Instead leave an 8-byte
  `Gfx foo_model[1]` in `.bss` and fill it at load with
  `gsSPBranchList(pool_body)` — `emu64.c:3496` `G_DL_NOPUSH` already implements
  the branch. Every `extern` keeps working, the address is a link-time
  constant, and the 9,931 `.data`→`.data` relocations then need **zero** runtime
  fixup. Exclusions that must hard-fail in the generator: `anime_6_model`
  (`emu64_print.cpp:105` range-checks it) plus ~14 symbols indexed as arrays.
- **S4 has 32,355 relocations, not 16,365.** All `R_SH_DIR32`. `dcasset`'s
  16,365 references and these are **disjoint sets** — `assets_scan.py` finds
  literal `pc_load_asset(` call sites, which exist only for `.bss`
  destinations. Reusable: the pack *format* and the window discipline. Not the
  extractor.

### Pool relief, which is worth more than `.bss` right now

`kb/STATE.md` caps S4's pool at ~498 KB and calls it the binding constraint.
The Dreamcast has **no JOY port and no GBA link cable**, so **222,568 B** of
GBA client/loader payload (`aBTD_island_prg/ldr`, `aNNW_client_prg/ldr`) never
needs pool bytes at all — **44.7% of the entire pool**, recoverable by dropping
them from `assets.pak`. Separately, `nintendo_hi_0` is declared `0x9900`
(39,168 B) but `src/static/boot.c:326-327` prints the real `.aw` size as
`0x66a0`: **12,896 B of pure slack**. It cannot simply be deleted —
`src/static/Famicom/famicom.cpp:2097` reuses the buffer.

## L8. Census finds — 663,472 B banked, 58,368 refuted

From an independent `nm -S` sweep of the clean non-stub ELF. `prbuf`,
`sys_stacks` and KOS `buffer.4` are **banked in A3**. All of them **stack with
L1/S4** — they are not asset destinations, so they survive the loader.

❌ **`CALLSTACK` + `pc_task_buf` (58,368 B) were REFUTED on implementation.**
The census read the base port's "tasks execute immediately inline. No message
queue, no thread" comment as meaning the buffers were dead. They are not:
`GetCallStack()` (`dvdthread.c:45`) hands out 128 × 0x100 B frames that
`DVDT_AddTask` fills and `pc_dvd_process_all_tasks()` drains, and `pc_task_buf`
is the live audio command buffer `RspStart2` consumes every frame
(`neosthread.c:35-39`). **A comment about threading says nothing about whether
the storage is live.** Do not re-propose these.

**Still live, unverified, lower confidence:**

| symbol | B | where | note |
|---|---:|---|---|
| `dvd_buf.3` | 65,536 | `jaudio_NES/internal/dvdthread.c:105` | double 32 KB DVD bounce. The census missed it *in the file it audited*. Given `CALLSTACK` was refuted in the same file, **check `__WriteBuffer`'s callers before believing this one** |
| `sys_dynamic` GBI arena | 132,104 | `include/sys_dynamic.h:27-35` | carries the smaller originals commented out, but shrinking risks a `THA_GA` overflow |
| ~~`m_bg_tex.c:3-34` `*_dummy`~~ | ~~33,792~~ | | ❌ **RESOLVED 2026-08-05, and the answer is the opposite of "placeholders": they are the LIVE destinations.** The dead weight was their 96 `mFM_grd_*` **sources**, 150,880 B, now demand-loaded — see A4. The 33,792 B of destinations must stay |
| `mCD_save_data_aram_malloc` | 147,840 | `src/first_game.c:24` | takes the three ARAM blocks from **libc `malloc`** — genuinely additive `sbrk` heap, permanent from boot. They must outlive scenes so `zelda_malloc` cannot hold them; they need a boot-time reservation inside the arena |

## L4. `.text` relocation — NOT NEEDED. Do not start this.

⚠️ **[CORRECTED 2026-08-06 — the premise is gone and the verdict is stronger.]**
~~`-O0` is mandatory, so `.text` (6,318,552 B) cannot shrink; it can only move.~~
**`.text` shrank to 2,854,108 B on a compiler flag (L0), so the item this lever
existed to relocate is now less than half its old size, and the image fits with
room to spare. Do not start this — it was already unnecessary, and it is now
unnecessary by a wider margin.** The rest of the entry stands as written.


MMU paging is **DEAD** (`kb/closed.md`). The surviving mechanism would be
**ScummVM-style code overlays** — a real, shipping SH-4 `R_SH_DIR32` ELF loader
(`backends/platform/dc/dcloader.cpp` + `plugin.x`, in production since 0.7.0).

**But the arithmetic says it is unnecessary:** L1 + L3 close the gap with
`.text` untouched. Earlier docs framed this as "the fork in the road for the
project" — that was wrong, and acting on it would be a large piece of work for
no required byte. Revisit **only** if L1 or L3 come in materially under their
measured estimates.

## L5. Offline asset decimation — not costed. **User's call, not engineering's.**

The only lever that shrinks the destination arrays *themselves* rather than
relocating them. Disc is 5.3% full and the target is 640×480. `src/data/model`
alone is 5,682,621 B of `.bss`. `PLAN.md` §1 already sanctions a documented "DC
edition". This is a product decision.

## L6. Source-level table dedup — MEASURED 2026-08-01. 915,139 B, mostly non-additive.

`tools/dcstub/measure_dedup.py`, run against the linked ELF and the real ISO:

```
python3 tools/dcasset/dcasset.py extract "<the ISO>" --out /tmp/discroot
python3 tools/dcstub/measure_dedup.py --rom /tmp/discroot     # + dc/build/dedup/*
```

| population | total | duplicate | share |
|---|---:|---:|---:|
| `.bss` asset destinations, keyed by **actual ROM bytes** | 8,503,550 | **794,640** | 9.3% |
| `.data` initialised tables | 2,589,975 | **120,499** | 4.7% |
| `.rodata` (symbol-visible part only) | 37,004 | 171 | 0.5% |

**Read the split before costing this.** The `.bss` 794,640 B does **not stack
with L1/S4**: once assets are demand-loaded those arrays do not exist, so the
saving evaporates. Its lasting value is elsewhere — 2,253 fewer distinct assets
to store in `assets.pak` and to stream off a 500 KB/s CD-R.

The `.data` 120,499 B is the durable part, and it splits again:

- **95,774 B (1,678 syms) aliasable** — distinct names, identical bytes.
  Realising it needs one object under two names. GNU aliases only work within a
  TU, so the generator must emit the canonical copies into a single generated
  TU and turn the duplicates there into `__attribute__((alias(…)))`. Biggest
  groups are `cKF_*_tbl` keyframe tables (184 copies of one 54-byte table, 304
  copies of a 26-byte one).
- **24,725 B (231 syms) redefined** — the *same* symbol name defined more than
  once, surviving only because the link carries
  `-Wl,--allow-multiple-definition`. One copy is reachable; the rest are dead.
  This is not aliasing, it is deleting redundant definitions, and it is the
  easier half. 1,367 data/bss symbols are multiply-defined overall.

**Correction to this lever's premise:** `src/data/**` is *not* generator output.
`gen_runtime_assets.py` **edits vendored decomp files in place** — that is where
the `#ifdef TARGET_PC` blocks came from. So a dedup pass cannot be "just a
generator change" in the sense L6 assumed. It can, however, use the mechanism
S1 proved: rewrite into a scratch tree under `dc/build/` and swap the TUs in
from `dc/Makefile`, exactly as `tools/dcstub/make_stub_data.py` does. `src/`
stays untouched.

**Verdict: keep, do not schedule.** 120 KB durable against an 8.27 MB gap, for
a nontrivial rewriter. It is worth doing *after* L3, or never. The measurement
existed to stop the question being asked again.

## L9. Our own libm dependency — 5,940 B, and it is in `dc/` code (2026-08-05)

Small, but it is the rarest kind of lever here: **`.text` that `-O0` does not
protect, because it is not `src/`'s.**

`dc/src/dc_misc.c:421` builds its sine table with the **double-precision**
`sin()`, and that single call drags in 44 % of the image's 13,400 B of libm:

```
k_rem_pio2.o 2720 · e_rem_pio2.o 1408 · k_cos.o 408 · s_scalbn.o 392 ·
s_floor.o 376 · k_sin.o 264 · s_sin.o 192 · s_frexp.o 148 · s_fabs.o 32
                                                        ------
                                                         5,940 B
```

The table is 1,025 `s16` entries of a quarter turn built once at startup, so the
argument reduction machinery those objects exist for is never needed. Any of:
`sinf()`, a float polynomial, or generating the table offline into `.rodata`
(2,050 B, which is a net loss unless the code goes too). **Not implemented.**

Related and already applied: `dc/src/dc_fmath.c` defines `sqrtf` itself, so
`libm_a-wf_sqrt.o` (84 B) and `libm_a-ef_sqrt.o` (240 B) are no longer pulled
in — **324 B**, on top of the speed win of not running a software square root
339 times a frame. The mechanism is archive extraction, not symbol overriding:
our objects precede `-lm` on the link line, so libm's copy is never reached.

## ⭐ L10. T1 — textures never reach the SH-4. **−579,248 B, then +2.78 MB of content for 68 KB** (designed 2026-08-06)

T1 was the highest-value open *concept* from 2026-08-01 until it graduated
into this entry. Designed against the tree today it is **smaller, better-placed and
cheaper than its own write-up**, and it is the first lever since R1 that frees
bytes instead of converting MISSING into PRESENT.

| | resident before | cost | **net** | delivers |
|---|---:|---:|---:|---|
| **phase 1** — the 669 case-1 textures (excludes NPC, segment-bound and file-static) | 618,048 | ~38,800 | **−579,248** | the same content, for 579 KB less |
| **phase 2** — extend the map to all 6,354 eligible | — | **+68,000** | +68,000 | **5,685 textures / 2,782,080 B that render as nothing today** |

**Phase 1 is a real saving, not a rule-8 content swap.** Phase 2 is the best
bytes-per-content ratio anywhere in this file: 68 KB for 2.78 MB.

### Why it is cheap — three findings, in order of how much they cut

1. **The seam is already ours.** `GXLoadTexObj` (`dc_gx.c:2288`) →
   `dc_gx_backend_texture_upload` (`dc_gx.c:2333` → `dc_pvr_texture.c:1060`).
   **No `src/` rewrite, no `make_src_shrink.py` rule, no `--wrap`** — strictly
   cheaper than R1's seam, which needed a rewriter to reach one `bcopy`.
2. **No N-slot pool is needed.** The PVR already holds every texture twiddled in
   VRAM behind a content-keyed LRU (`uploads=306 hits=894442 evictions=0`). The
   main-RAM array is read on every bind **only to compute the cache key**
   (`tex_content_hash`, `dc_pvr_texture.c:1092`, ~109 binds/frame). Replace that
   key with a synthetic one built from the asset row and the array is needed
   **only on a miss** ⇒ **one 24,576 B staging buffer**, ~38,800 B all in.
   R2/R3's 16-slot machinery is not required here.
3. **The population is tiny and uniform.** Max texture 4,096 B with a single
   outlier (`FONT_nes_tex_font1`, 24,576 B — which is why the staging buffer is
   that size); **99.4 % are ≤ 2,048 B**. Every texture is `rom_src=0`, `swap=0`,
   i.e. a pure `pread` with no byte-swap.

### The inventory it rests on

Method: `make_stub_data.py`'s own `IFDEF_RE`/`DECL_RE`, cross-checked because it
reproduces this file's independently-derived acre figure of **815,024 B
exactly**.

| population | total | resident |
|---|---:|---:|
| all asset destinations | 8,813,054 B / 16,341 syms | 1,885,176 B / 1,742 syms |
| of which textures | 5,053,824 B | **752,640 B** |

### The hazard, and it is smaller than it looks

27 of 8,761 `gsDPSetTextureImage_Dolphin` sites use **pointer arithmetic**. All
27 are in `src/data/model/hnw_model.c`, all of the form `anime_4_txt + 0x…` —
and `anime_4_txt` is `SEGMENT_ADDR(0x0B,0)`, **not a `.bss` symbol**, so it
resolves through `gSPSegment` and is safe. Mitigation as specified: exclude the
14 `gSPSegment`-argument symbols (1 resident, 512 B) and all of
`src/data/npc/**` (R2's domain).

### ⚠️ The falsification experiment — RAN 2026-08-09, AND THE PROBE WAS BROKEN

`DC_TEXPOOL_PROBE=1`, counters `interior` / `mutated` / `oversize` / `aliased`.
**Any one non-zero kills the design as specified.** It ran, twice, and the
result is a lesson about instruments rather than about T1:

| run | scenes reached | verdict |
|---|---|---|
| 300 s | `0 → 3 → 4` (**never reached the town**) | `interior=0 …` over 726,570 binds — clean for the wrong reason |
| 700 s | `0 → 3 → 4 → 18 → 9` | 🔴 `interior=4318` over 2,187,050 binds — **a probe bug** |
| 700 s, probe fixed | `0 → 3 → 4 → 18 → 9` | ✅ **`interior=0 mutated=0 oversize=0 aliased=0`** over **2,074,009 binds / 127 distinct textures** |

🔴 **AND THE 4,318 ARE A PROBE ARTIFACT, NOT A PROPERTY OF THE GAME.** Every one
named a single symbol — `ef_doyon01_00`, at +68/+324/+480. That symbol is **not
in `keeplist-town.txt`**, so the stub tree declares it `u8 ef_doyon01_00[1]`
while `dc_texpool_map[].size` still carries its real 1,024 B. The linker packs
~50 other small symbols into the window after it (`dna_win_*_pal`,
`ef_ame02_*_v`, `ef_anahikari01_*`, all in 0x8c684xxx), and every bind to one of
THOSE was attributed to `ef_doyon01_00` as an interior pointer.

⭐ **`dc_texpool.c`'s own header states the premise — "under `DC_ASSET_STUB` an
unkept array is `u8 x[1]`, so an interior pointer into it is a pointer into
WHATEVER THE LINKER PUT NEXT" — and then the containment test used the declared
size anyway.** Fixed 2026-08-09: a row with `kept == 0` is one byte long, so
anything past its base is `unmapped`, not `interior`. **Every `interior=`
figure printed before that fix is void, and the design is neither cleared nor
killed until the re-run lands.**

### ✅ VERDICT 2026-08-09: T1 IS CLEARED TO BUILD

`smoke-texprobe3-20260809-142619`, **deepest scene 9**, 17,609 frames:

```
[DC/TEXPOOL] VERDICT interior=0 mutated=0 oversize=0 aliased=0 (all four must be 0)
[DC/TEXPOOL] binds=2074009 mapped=560487 interior=0 unmapped=1513522 distinct=127
```

**All four killers zero over 2.07 M binds with the town exercised.** So, as
specified: no bind lands inside a mapped array, no mapped array's content
changes between binds, the decoder never reads past a row, and no two rows share
an address. **A synthetic identity key is a legal substitute for the content
hash, and T1 needs ONE ~24,576 B staging buffer rather than an N-slot pool.**

⚠️ `unmapped=1513522` (73 %) is expected and must not be "fixed" — those are
textures reached through a pointer rather than a static display-list operand
(`FONT_nes_tex_font1` is the type case). It bounds what a static operand scan
can account for; it is not a hole.

⚠️ **Still unproven, and it is the PLAYABILITY risk rather than the correctness
one: the seeks.** T1 issues ~306 per run = **6-30 s on CD-R**, as mid-scene
hitches. `dc_keep_sweep()` already implements the read-ahead window that
collapses ~306 seeks to ~40, **and R1 still does not use it either.** Build the
shared helper first; do not ship T1 without it.

⚠️ **TWO PROCESS LESSONS, both already in this kb in other words:**
1. **A short run is not a cheap run, it is a DIFFERENT run.** The 300 s pass
   returned a clean verdict because it never entered the town. `kb/RESUME.md` §8
   says the same thing about the census only seeing depth-0 branches — **check
   `deepest_scene` before believing any "all clear".**
2. **`interior` is only meaningful for RESIDENT rows** — and resident rows are
   the only ones T1 takes bytes back from anyway.

### ⚠️ The seek risk, and the fix that already exists in the tree

T1 issues **one seek per distinct texture, ~306 per run** = **6-30 s of seeks on
hardware**, concentrated as mid-scene hitches — the one thing that could make
this a regression a human notices. Resident texture ROM offsets are **clustered**
(median gap 512 B; 863 of 905 gaps ≤ 32 KB), so a **32 KB read-ahead window
collapses ~306 seeks to ~40**.

**`dc_keep_sweep()` (`dc_main.c:977-1108`) already implements exactly that
window discipline — and R1 does not use it**, which is why R1 still pays 27
unbatched seeks per acre load. Fixing R1 and building T1 want the same helper.

⚠️ **Do NOT reach for a wholesale sorted prefetch instead.** Resident texture
offsets span **10.9 MB**, i.e. ~22 s of linear read.

## L7. Bucket 6's high-water mark — deferred, deliberately.

Still unmeasured. Recipe (from the deleted bucket-6 research): instrument the
Anbernic host build, drive from a late-game save, report `__osMalloc` peak /
`JKRExpHeap` peak / `largest_free`). It **blocks nothing** now: A1 cut the arena
by exactly its dead weight, so the pool is unchanged.

**Defer until the image is within ~1 MB of fitting**, at which point the last
megabyte has to come from somewhere and the peak decides whether the arena can
give it up. Do not spend a session on it before then.
