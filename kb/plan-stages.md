# The staged RAM plan — S1 → S5

The agreed sequence for closing the RAM gap, with the reasoning behind each
step so a fresh context does not re-derive it. **The ordering was the user's
call on 2026-08-01: execute it, do not re-litigate it.** Current status and the
concrete next moves live in `kb/STATE.md`; the costed solution stack for S3's
remainder and S4 is `kb/ram-plan.md`; the ranked ledger is `kb/levers.md`.

> ## ⚠️ [2026-08-06] A LEVER THIS PLAN EXPLICITLY FORBADE WAS TAKEN, AND IT WORKED
>
> The `-O0` directive was reversed by the user. `src/` builds at `-Os` with a
> 14-TU `-O3` hot list (`DC_OPT_PROFILE=perf`); `dc/src` moved `-O2` → `-O3`.
> Measured, matched town windows:
>
> | | `-O0` | `-Os` | `-Os` + `-O3` hot | + `dc/src` `-O3` |
> |---|---:|---:|---:|---:|
> | `.text` | 5,506,964 | 2,680,676 | 2,729,152 | **2,753,700** |
> | `.data` | 2,337,980 | 2,224,832 | 2,224,832 | **2,224,832** |
> | `.bss` | 3,945,356 | 3,945,484 | 3,945,484 | **3,945,484** |
>
> Evidence: `kb/state-log.md`, top entry, 2026-08-06.
>
> **The S1→S5 ordering itself SURVIVES, and two of its findings got stronger:**
>
> - **`.bss` did not move (+128 B).** S4/L1 is still the binding move and still
>   the critical path. No compiler flag reaches asset destination `.bss`.
> - **Finding 1 — "`.text` overlays (L4) are NOT needed" — is now much more
>   securely true.** `.text` fell 2,753,264 B for free, which is far more than
>   any overlay scheme was ever costed at. The `R_SH_DIR32` loader is further
>   from being needed than when this was written.
> - **Finding 2 — "the asset pool is the binding constraint" — stands**, and the
>   pool ceiling should now be *re-derived* upward: the image-span side of the
>   inequality gained ~2.75 MB of slack that the ~498,250 B pool ceiling was
>   computed without. **Re-derive it from a current link before sizing S4's
>   pool; do not reuse the number in "Finding 2".**
>
> **What does NOT survive is this file's closing instruction — see the bottom.**

## Next actions — the agreed plan

**User chose this sequence on 2026-08-01 (S1 → S4, in order).** Do not
re-litigate the ordering; execute it. The reasoning behind each step is below
so a fresh context does not have to re-derive it.

**`kb/ram-plan.md` (2026-08-01) is the costed solution stack for S3's remainder
+ S4** — eight moves with closing arithmetic, gates, and the experiment queue.
Execute S4 from it.

### The two findings that shaped this plan

1. **`.text` overlays (`kb/levers.md` L4) are NOT needed.** The gap closes
   without touching `.text`. L4 was previously written up as "the fork in the
   road for the project" — that framing was wrong. Do not spend a session on
   the ScummVM `R_SH_DIR32` loader unless the arithmetic below stops holding.
2. **The asset pool is the binding constraint, not the arrays.** L1 removes
   8,771,358 B of `.bss` (→ 3,644,150), but the pool it loads *into* is
   additive heap. Solve the inequality for it: **the pool can be at most
   ~498,250 B** unless something else also moves. That is uncomfortably tight
   for streaming 8.9 MB of assets, which is why S3 comes before S4.

### Complication to budget for in S4

L1 is billed as "loader-only, no codegen". True, but it understates the work.
The destination arrays are referenced **by address** from initialised `.data`:

```c
Vtx glider_v[0xB0 / sizeof(Vtx)];                          /* .bss dest   */
Gfx glider_model[] = { … gsSPVertex(glider_v, 11, 0) … };  /* .data, baked ptr */
```

Pooling the storage means **fixing up every such reference at load time** —
`dcasset`'s round trip already replays **16,365** of them, so the tool has the
data, but the loader must apply relocations, not just `memcpy`.

### The structural risk S1 existed to kill — RETIRED, S1 killed it

*(Kept for the record. This was true until 2026-08-01; the section at the top of
this file is what replaced it.)*

**Zero lines of this port have ever executed.** Not on hardware, not in
Flycast. The boot failure is size alone, so `dc_main.c`'s trampoline, KOS init,
the platform layer, the GX stubs and `dc_mem_ledger.c`'s new `MEMLEDGER FIT`
line have never run once. Every RAM estimate assumes a platform layer nobody
has observed working. If S4 lands after a week and *then* the trampoline turns
out to be broken, the two failures are tangled and hard to attribute.

---

### S1. Stub the assets and BOOT IT. ✅ DONE 2026-08-01 — evidence in `kb/state-log.md`.

Landed as `tools/dcstub/make_stub_data.py` + `DC_ASSET_STUB=1`. The image boots
and reaches `JW_Init2`; it stops on an empty `/cd`, not on a platform fault.
**⭐ Start at S2.** The original write-up follows, unchanged, because it is the
argument for why the step was worth taking.


Build a **throwaway** image with the destination arrays sized `[1]`: a
`DC_ASSET_STUB` build mode that rewrites generator output into a scratch tree.
**No `src/` edits, nothing committed to the real tree** — `src/data/**/*.c` is
output of `pc/tools/gen_runtime_assets.py`, so regenerating small is a
*generator* change, legal under the ~~`-O0` rule~~ **no-`src/`-edits rule** (the
`-O0` half was reversed 2026-08-06; the no-edits half was not, and it is the
half this argument actually needed).

`.bss` → ~3,644,150, under the 4,143,556 ceiling with ~500 KB spare. **The
image fits and should boot.**

The game renders garbage the moment it touches an asset. That is expected and
fine. What S1 buys is the first execution of the trampoline, KOS init, the
console path, `MEMLEDGER FIT`, and `crash.sh` symbolising a real fault —
surfacing every platform-layer bug *now*, separately from the loader.

Cost: small. Buys: the largest available reduction in unknown-unknowns, plus
the first end-to-end validation of the harness. Throwaway once S4 lands.

### S2. Measure L6 — generator table dedup. ✅ DONE 2026-08-01. 915,139 B, mostly non-additive.

`tools/dcstub/measure_dedup.py --rom <dcasset extract dir>`. Full numbers and
the verdict are in `kb/levers.md` L6. Headline: `.bss` asset destinations are
9.3% duplicate by actual ROM bytes (794,640 B) but that **evaporates when S4
lands**; `.data` is 4.7% duplicate (120,499 B) and that part is durable.
**Verdict: keep, do not schedule.** Two corrections it produced —
`src/data/**` is *not* generator output (`gen_runtime_assets.py` edits vendored
decomp in place), and 1,367 data/bss symbols are multiply-defined, surviving
only on `-Wl,--allow-multiple-definition`. The original write-up follows.


`src/data` is generator output; hashing table contents and aliasing duplicates
in `gen_runtime_assets.py` is a generator change, not codegen. **Nobody has
looked.** Could be 0, could be megabytes.

Cost: small — a host-side hash pass over the generated tables gives the number
without changing the build. Worth doing purely because the answer is cheap and
currently unknown.

### S3. Bank the independent savings. ✅ **DONE — 2,591,016 B banked** (2026-08-01 + 08-02).

Was billed as "six measured, mutually independent moves, ~4.3 MB". **All three
parts of that description were wrong** — the total is 2,928,267 B, the moves are
not independent, and every individual estimate was off. `kb/levers.md` L3 now
carries the re-costed table. Banked so far (commit `b0e009d`):

| pass | `.bss` | mechanism |
|---|---:|---|
| `tools/dcstub/make_src_shrink.py` | −1,159,392 | 7 literals, scratch-tree rewrite, `DC_SRC_SHRINK` |
| `dc_gx` + `dc_os` | −278,796 | vertex 8192×40 → 2040×32; hand-rolled `ocbp` loop |
| `pc_m_card` | −308,234 | delete a double buffer, retype, move to the arena |
| `make_src_shrink.py` S6 (2026-08-02) | −598,424 **image, not `.bss`** | `s_assets[].path` + its 14,495 string literals deleted |
| `make_src_shrink.py` S7 (2026-08-02) | −246,064 **image, not `.bss`** | `data_bgd[].collision` run-length coded; `.data` −300,896, `.rodata` +54,832 |

**`s_assets[]` name strings: ✅ BANKED 2026-08-02, −598,424 B of image**
(`.rodata` −598,648, `.text` +224, span −598,112). `make_src_shrink.py` rule
**S6** deletes the `const char* path` field and its 14,495 `"assets/….bin"`
literals from `pc/src/pc_assets.c` via the existing scratch-tree swap; the
five live fields stay. No `dcasset gentable` was needed — the strings' only
consumer is a `.bin` `fopen` fallback that cannot be reached on DC.
**The −821,569 B estimate was 223,145 B too high**: it counted the 347,880 B
`s_assets[]` table, which is live, as string pool. See `kb/levers.md` L3
"Correction 0". `DC_SRC_SHRINK=0` still reverts everything.

**`data_bgd` collision split: ✅ BANKED 2026-08-02, −246,064 B of image** —
`.data` −300,896, `.rodata`/`.text` +54,832, `.bss` unchanged. Rule **S7**
run-length codes the 295 acre collision maps (317,420 B of `.data`, 95.2 % of it
the `collision[16][16]` member) against a 380-entry palette and expands them at
the single call site that reads them, `m_field_make.c:271`, which walks them
sequentially into a heap-resident copy once per block load. Verified by decoding
the palette and stream back out of the *linked* ELF and comparing all 295 maps
against the previous build bit for bit.

Two corrections it produced: **"collision" in the name means the collision
*map*, not a symbol collision** — `data_bgd` is singly defined and is not part
of the 1,367-symbol `--allow-multiple-definition` family; and the saving is in
`.data`, not `.bss`. The −236,544 estimate had no derivation anywhere in the
repo and came in **9,520 B under** the measured result, the only row so far to
be beaten rather than missed. Plain dedup of identical maps was measured too and
is worth only 38,912 B — the saving is in the run-length structure, not
duplicate acres.

**S3 is complete.**

### S4. Build the loader — `kb/levers.md` L1 + L2. ⭐ NOW THE CRITICAL PATH.

`pc_assets.c` against `assets.pak`. Contract: `kb/asset-pack.md`. Four things
learned since this step was written, all of which change how it must be built:

1. **32,355 relocations, not 16,365.** All `R_SH_DIR32`. `dcasset`'s 16,365
   references and these are **disjoint sets** — `assets_scan.py` finds literal
   `pc_load_asset(` call sites, which exist only for `.bss` destinations.
   Reusable: the pack *format* and the window discipline. Not the extractor.
2. **Use branch trampolines, not pointers.** Turning `Gfx foo_model[]` into
   `Gfx *foo_model` changes the symbol's *type* and requires rewriting **1,325
   `extern Gfx x[];` sites** in hand-written decomp, silent on failure. Instead
   leave an 8-byte `Gfx foo_model[1]` in `.bss` filled at load with
   `gsSPBranchList(pool_body)` — `emu64.c:3496` `G_DL_NOPUSH` already
   implements the branch. Every `extern` keeps working, the address is a
   link-time constant, and 9,931 of the relocations then need zero runtime
   fixup. Hard-fail exclusions: `anime_6_model` (`emu64_print.cpp:105`
   range-checks it) plus ~14 symbols indexed as arrays.
3. **Textures probably should not be pooled at all.** ~4.6 MB of the 8.5 MB of
   destinations is texture data whose only consumer is the PVR. Pooling it pays
   for those bytes twice. See `kb/research-creative-ram.md` T1 — this is the
   highest-value open idea in the project and it should be settled *before* the
   pool is sized.
4. **The pool may not need to be a separate extent at all.** `research-creative-ram.md`
   T4: every pool byte is pack-backed and therefore evictable, so the pool can
   live in the arena's tail and share slack with bucket 6's unknown peak.
   This is a decision to take before the loader's allocator interface is
   written.

### S5. The remaining gap, honestly stated.

After S3's banked 1,810,816 B the image is **6,999,924 B over**. L1 (asset
demand-loading) measured **8,460,128 B** of `.bss` recovery in the stub
experiment, so S4 alone is arithmetically sufficient *if* its pool stays under
~1.46 MB. That is the whole ballgame, and it is why T1 and T4 above matter more
than any further `.bss` trimming.

---

~~**Be honest in reporting.** "Still N MB short with `-O0` mandatory" is a valid
and important result. If the levers do not close the gap, cutting content
(`kb/levers.md` L5 — the user's call, not engineering's) or declaring a
stock-16 MB build infeasible are the honest options; quietly reopening the
optimization question is not.~~

🔴 **[REWRITTEN 2026-08-06 — this paragraph was the plan's single most costly
sentence.]** It told every future session that the optimization question was
not merely settled but *dishonourable to raise*, so nobody re-read what the ban
rested on: one unreproduced armhf session, never tested on SH-4, in which
`-O1` was never isolated from `-mcpu=cortex-a53 -mfpu=neon-vfpv4` and the
failure was blamed on unaligned LDRD/VFP — **which cannot happen on SH-4**. The
question was reopened openly, by the user, on maintainer advice, and it was
worth `.text` 5,506,964 → 2,753,700 B and town FPS 11.6 → 20.6.

**What replaces it.** Be honest in reporting: "still N MB short" is still a
valid and important result, and cutting content (`kb/levers.md` L5) or declaring
a stock-16 MB build infeasible are still honest options. But the rule is now the
opposite of the old one — **raising a closed question OPENLY, with the evidence
it rests on quoted back, is exactly what is wanted; what is forbidden is
changing the build quietly.** Optimization is a live, kill-switched lever:
`DC_OPT_PROFILE=perf|size|o0`, lists in `dc/opt-lists.mk`, `o0` a
byte-identical revert. Evidence: `kb/closed.md`'s `-O0` post-mortem and
`kb/state-log.md`, 2026-08-06.

