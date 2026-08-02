# RAM levers — the ranked ledger

Every way found so far to close the RAM gap, with status. **Read this before
planning any size work.** Read `kb/closed.md` before *proposing* any — several
obvious ideas are already dead.

Current gap: **8,273,108 B** over; the `.bss` ceiling is **4,143,556 B** against
12,415,508 today. See `kb/STATE.md` for the inequality and **for the agreed
S1–S4 execution order** — this file is the ledger, that file is the plan.

Two results that reorder this list, both derived in `kb/STATE.md`:

- **L4 (`.text` overlays) is NOT needed.** The gap closes without touching
  `.text`. Earlier docs called it "the fork in the road"; that was wrong.
- **L1's pool is the binding constraint.** L1 lands `.bss` at 3,644,150, but
  the pool it loads into is additive heap and may be at most ~498,250 B unless
  L3 also lands. L1 alone is not really sufficient.

Only **layout** levers are legal. `-O0` is a user directive, so anything that
changes instruction selection is banned:

| Lever | Changes instruction selection? | Allowed |
|---|---|---|
| `-O1/-O2/-Os`, LTO, `-mrelax` | yes | **no** |
| `.bss` right-sizing, arena sizing | no | yes |
| Moving data/code to `/cd`, demand loading | no | yes |
| Linker script placement, code overlays | no | yes |
| Offline asset conversion / decimation | no | yes |

---

## Applied

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

  ⚠️ `kb/research-budget-premises.md` §2.2 advises handing back a **non-NULL
  dummy**. That advice was deliberately **not** followed: a small dummy leaves
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

## L3. The ranked remainder — ~4.3 MB total

From `kb/research-size-reduction.md`, measured against the real ELF + map:

| item | saving |
|---|---:|
| `.data` `src/data` tables to disc (0.95 MB pointer-free today; 0.99 MB needs a REL-style reloc pass) | −1.94 MB |
| `s_assets[]` name-string pool → disc index | −0.89 MB |
| `audiomemory`/jaudio → AICA | −0.65 MB |
| actor overlay staging arenas → one shared union arena | −0.46 MB |
| `pc_m_card` | −0.28 MB |
| `dc_gx` | −0.24 MB |
| emu64 `texture_buffer_data` → VRAM | partly taken in A2 |

## L4. `.text` relocation — NOT NEEDED. Do not start this.

`-O0` is mandatory, so `.text` (6,318,552 B) cannot shrink; it can only move.
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

## L6. Source-level table dedup — nobody has looked.

`--icf` is unavailable on SH (`kb/closed.md`), but `src/data` is **generator
output**. Hashing table contents and aliasing duplicates in
`gen_runtime_assets.py` is a *generator* change, not codegen — legal under the
`-O0` rule. Unknown saving.

## L7. Bucket 6's high-water mark — deferred, deliberately.

Still unmeasured. Recipe: `kb/research-budget-premises.md` §2.4 (instrument the
Anbernic host build, drive from a late-game save, report `__osMalloc` peak /
`JKRExpHeap` peak / `largest_free`). It **blocks nothing** now: A1 cut the arena
by exactly its dead weight, so the pool is unchanged.

**Defer until the image is within ~1 MB of fitting**, at which point the last
megabyte has to come from somewhere and the peak decides whether the arena can
give it up. Do not spend a session on it before then.
