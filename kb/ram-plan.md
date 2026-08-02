# RAM plan — the solution stack that closes the 6,999,924 B gap

Written 2026-08-01. **Documentation only — nothing in this file is implemented.**
This is the execution-ready answer to "how does the port fit in stock 16 MB",
assembled from `kb/levers.md`, `kb/research-creative-ram.md`,
`kb/research-budget-premises.md` and `kb/asset-pack.md`. It does not re-litigate
the agreed S1→S4 order in `kb/STATE.md`; it details what S3's remainder and S4
must contain, and what backs them up if their numbers erode.

Rules honoured throughout: `-O0` on `src/` (layout levers only), no `src/`
edits (scratch-tree rewrites + prelude only), every change gets a kill switch,
stock 16 MB, and every idea was checked against `kb/closed.md` before listing.

---

## 0. The target

```
(image span) + (genuinely additive heap) ≤ 16,646,144
  image span   19,564,308   (.text 6,320,700 / .data 2,638,852 / .bss 10,669,268)
  additive      4,081,760   (KOS 262,144 + arena 2,705,504 + ARAM window 1,048,576 + threads 65,536)
  over by       6,999,924
```

`.text`+`.data` cannot shrink (`-O0`); they can only be *relocated* or their
consumers moved. `.bss` must fall ~67% (ceiling 3,604,832). One inequality,
never two pools — see `kb/STATE.md`.

---

## 1. The core stack — eight moves, ranked, with the closing arithmetic

Execute in this order. P6/P7 are the S3 remainder; P1–P5 and P8 are S4's
content. Each row states which side of the inequality it moves.

### P1. S4 loader with branch trampolines — `.bss` −8,460,128 [measured, stub experiment]

`kb/levers.md` L1+L2. `pc_assets.c` rewritten against `assets.pak`
(`kb/asset-pack.md` is the contract): destination arrays become 8-byte
`Gfx foo_model[1]` stubs in `.bss`, filled at load with
`gsSPBranchList(pool_body)` — `emu64.c:3496` `G_DL_NOPUSH` already implements
the branch. Every `extern` keeps working, addresses stay link-time constants,
and 9,931 of the 32,355 `R_SH_DIR32` relocations need zero runtime fixup.

- Generator must **hard-fail** on the known exclusions: `anime_6_model`
  (`emu64_print.cpp:105` range-checks it) plus ~14 symbols indexed as arrays.
- L1 is undercounted by 159,037 B (12 files whose bounds are macros or which
  are `.c_inc` under `include/` — `kb/levers.md` L3 correction 3). Fold them in.
- Reconcile the 769-vs-768 `_pc_load_src_*` count
  (`kb/research-creative-ram.md` doc-error 4) **before** trusting the replay's
  completeness — the pack's safety argument rests on it.
- Kill switch: the loader ships behind `DC_ASSET_POOL=0` → old eager path.

### P2. Textures are never pooled — pool relief ~4.4–4.6 MB [claim; 1 h experiment]

`kb/research-creative-ram.md` T1, the highest-value open idea. Of the 8.5 MB of
destinations, ~4,629,656 B (10,035 `*tex*` symbols) + 97,044 B of TLUTs have no
consumer but the PVR. Pooling them pays twice (pool copy + VRAM copy). Instead:
loader streams a texture chunk into a transient scratch (≤ `max_chunk_bytes`
137,856 B), converts, uploads via `dc_gx_backend_texture_upload`
(`dc/src/dc_gx.c:2052`), records `dest_addr → PVR handle`, frees the scratch.
The trampoline stub keeps every `extern` valid; `dc_gx` resolves pointer →
handle.

- **Refinement (new, offline):** extend `dcasset` to emit those chunks
  pre-twiddled/VQ so the runtime does zero conversion and the upload is a
  straight `pvr_mem` copy. VQ is ~1/8 of 16-bpp — the 4.6 MB of destinations
  becomes roughly 0.6–1.2 MB of VRAM. Precedent: dca3 ships GTA3 on 16 MB
  exactly this way (VQ textures resident only in VRAM). Fallback for formats
  that resist offline conversion: runtime twiddle from the scratch, as above.
  Offline asset conversion is explicitly a legal layout lever.
- Gate experiment (~1 h, run first): grep `src/` for references to the 20
  largest `*_tex` symbols that are not `GXInitTexObj`/`gsDPSetTextureImage`.
  Count ≈ 0 ⇒ concept alive. Known CPU-side reader to special-case:
  `famicom.cpp:2097` reuses `nintendo_hi_0`.
- Debug detector: poison-fill stubbed texture arrays; assert `dc_gx` never
  receives poison as pixel data (no MMU — silent wrong reads must be made loud).

### P3. The pool is an evictable cache in the arena tail — additive heap loses the pool line [design decision]

`kb/research-creative-ram.md` T4. Every pool byte is pack-backed ⇒ reloadable ⇒
evictable by construction. Allocate the pool from the arena's tail; register an
eviction callback on game-heap allocation failure. The pool and bucket 6's
unknown peak then share one slack instead of double-counting two margins, and
the additive-heap line drops the pool entirely.

- **Must be decided before the loader's allocator interface is written** — it
  changes the API (`pool_alloc` needs an evict path and pin/unpin for
  in-flight assets).
- Detectors: JKRHeap free-size/largest-block log per scene transition +
  eviction counter. Eviction storms during scene loads are the failure mode;
  P2 (textures out) and P5 (GBA out) shrink the pool's working set first.
- If rejected: fallback ceiling for a separate additive pool is 1,460,204 B
  post-P1 (2,508,780 B once P4 lands) — still viable, just wasteful.

### P4. ARAM graph window moves to VRAM — additive heap −1,048,576 [claim; bench exists]

`kb/research-creative-ram.md` T2. `dc/src/dc_aram.c` is a single seam; every
window touch is a bulk memcpy inside `ARStartDMA` (lines 181–185). Replace
`dc_mem_alloc(DCMEM_ARAM_GRAPH, …)` at `dc_aram.c:71` with
`pvr_mem_malloc(window_size)` and the two memcpys with 32-bit-aligned,
SQ-assisted copies. Pays back the 536,576 B self-inflicted window-doubling debt
(`kb/STATE.md`), and in VRAM the window can grow to 2–4 MB at zero main-RAM
cost — `forest_2nd.arc`'s graph half fits, the `rebases=11` thrash dies, and
PLAN §3.1's disc-backed LRU becomes nearly trivial.

- Gate experiment: compile and run `harness/dc/bench/bench_mem.c` — it already
  exists, probes every main-RAM↔VRAM path both directions with checksums, and
  has **never been run**. It also settles the project's one missing number
  (SH-4-from-VRAM read bandwidth; a 2026-08 web pass found no citable figure
  either — the bench is the source of truth). Even a pessimistic 10–15 MB/s
  read rate is 10–30 ms inside a load transition, not a frame cost.
- VRAM budget check before growing past ~1 MB: 8 MB − framebuffers (~1.2 MB) −
  TA buffers − P2's texture set. Size after P2's numbers exist.
- Kill switch: `DC_ARAM_IN_VRAM=0` reverts to the main-RAM window.

### P5. Drop the GBA payloads from the pack — pool relief 222,568 B [measured]

`kb/levers.md` L3 "pool relief". The Dreamcast has no JOY port and no GBA link:
`aBTD_island_prg/ldr`, `aNNW_client_prg/ldr` can never be sent anywhere. Drop
them from `assets.pak`; their trampoline stubs load a zero-length marker and
the loader logs-and-zero-fills if anything ever requests them (loud, not
silent). 44.7% of the old 498 KB pool ceiling, recovered offline.

### P6. `s_assets[]` name strings — ✅ **DONE 2026-08-02, `.rodata` −598,648** (image −598,424)

Landed as `make_src_shrink.py` rule **S6** + `PC_REUSE_C` run through
`$(shrinkify)` in `dc/Makefile`. Deletion, as this plan said — no `dcasset
gentable` was needed, because there is nothing left to look up: the strings'
only consumer was `pc_load_asset()`'s `.bin` `fopen` fallback, and that fallback
is unreachable on DC (all 14,495 rows are `rom_src` DOL/REL, `dc_dvd.c` only
implements the ROM-direct path, and the relative `assets/…` paths resolve
against KOS's `/` where nothing is staged). The table index is the diagnostic
identifier now.

**The −821,569 estimate was wrong by 223,145 B** — it counted the live 347,880 B
`s_assets[]` table as if it were string pool. Measured across two clean full
rebuilds differing only in the rule: `.rodata` 1,057,364 → 458,716, `.text`
+224, `.data`/`.bss` unchanged, span `0x12e81c0` → `0x1255f60`. Derivation and
the full liveness argument: `kb/levers.md` L3 "Correction 0".

### P7. `data_bgd` collision split — `.data` −236,544 [measured; S3 remainder]

The S3-eligible slice of the `.data src/data → disc` row (`kb/levers.md` L3).
The rest of that row is P8 and belongs inside S4.

### P8. `.data` display-list bodies into the pool — `.data` −901,300 [measured; inside S4]

Strictly downstream of P1: the 1,014,088 B of `Gfx` bodies relocate to pool
addresses that do not exist until S4 assigns them (`kb/levers.md` L3,
structural correction 1). Under P3 these bytes become evictable pool content,
not additive heap. Note: landing this makes L6's 95,774 B aliasable-`.data`
saving worth ~0 (mutually exclusive — L3 correction 2).

### The closing arithmetic

| step | side moved | Δ | over by |
|---|---|---:|---:|
| start | | | **6,999,924** |
| P6 `s_assets` strings ✅ | image (`.rodata`) | −598,112 span (was billed −821,569) | 6,401,812 |
| P7 `data_bgd` split | image (`.data`) | −236,544 | 6,165,268 |
| P1 loader | image (`.bss`) | −8,460,128 | **−2,294,860** (margin 2,294,860) |
| P4 ARAM window → VRAM | additive heap | −1,048,576 | margin 3,343,436 |
| P8 DL bodies → pool | image (`.data`) | −901,300 | margin ≈ 4.24 MB |
| pool cost | additive (0 under P3; ≤1.46–2.5 MB if separate) | | **fits either way** |

⚠️ **The `start` row is stale in the other direction.** It is the 2026-08-01
image. The PVR backend landing in `dc/src` since then has grown the image
independently of anything in this table — a clean rebuild on 2026-08-02
measured the pre-P6 span at **19,824,576**, i.e. **+260,268** on the 19,564,308
this chain starts from. Re-baseline before trusting the absolute column; the Δ
column is what each row actually owns.

**The gap closes with margin even if P3 is rejected and the pool stays a
separate additive extent.** P2 does not appear as a row because its value is
pool-side: it halves what the pool must hold (what remains is dominated by
3,021,392 B of `Vtx` the SH-4 genuinely reads per frame; per-scene residency
plausibly ≤ 1–1.5 MB — the N64 ran this game in 4 MB total).

**The margin is already spoken for — do not treat it as spare:**

| claimant | plausible cost |
|---|---:|
| bucket 6 truth (arena may need 3.5–5.5 MB vs 2,705,504 now; the dead `0x380000` default argues ~3.5 MB) | +0.8–2.8 MB |
| disc read-ahead ring (bucket 10 is a phantom today — no ring exists) | ~64–384 KB |
| GLdc at M2 (vertex-buffer growth unverified) | unknown, ~0.1–0.5 MB |
| audio stage A/B buffers, save/VMU staging, unknown unknowns | ~0.2–0.5 MB |

---

## 2. Second-rank levers — bank when cheap, or when margins erode

Ordered by bytes-per-risk. None is required by §1's arithmetic.

| lever | Δ | status / gate |
|---|---:|---|
| T5 boot-tail reclaim (donate init-only `.text` to the pool: `_pc_load_src_*` 87,736 + `pc_assets.c` rest 10,088 + one-shot bswap 4,824) | −102,648 floor, 150–300 KB plausible | linker-script wildcard; guard build fills donated region with illegal opcodes so late calls fault loudly. Resolve which loaders stay lazy under S4 first |
| T6 `dvd_buf.3` double 32 KB DVD bounce (`dvdthread.c:105`) | −49,152…−65,536 | 10-min liveness check of `__WriteBuffer` callers **first** — same file produced two refuted claims |
| L8 `mCD_save_data_aram_malloc` (`first_game.c:24`) | moves 147,840 from permanent libc `sbrk` heap into a boot-time arena reservation | must outlive scenes; reservation, not `zelda_malloc` |
| T7 soft additive reserves (`threads` 65,536→32,768; KOS 262,144 round-up vs 216,704 measured) | −80–110 KB additive | ledger constants in `dc/` headers; `MEMLEDGER FIT` is the detector |
| L8 `m_bg_tex.c` `*_dummy` placeholders | −33,792 | unverified — check readers |
| L8 `sys_dynamic` GBI arena | −(part of 132,104) | risky: `THA_GA` overflow; needs a high-water probe before any cut |
| T8 OC-RAM (`.ocram` at `0x7c001000`): rspsim `DMEM[0x1000]` or a stack | −4–8 KB + speed | halves operand cache — measure before shipping |
| **pack-chunk LZ compression (new)** | 0 RAM; effective disc rate ~2–3× | Not the closed "compress `1ST_READ.BIN`" item — that saved 0 because `.bss` is `NOBITS`. This compresses `assets.pak` *chunks* so demand-refills and boot stream faster off a 500 KB/s CD-R (17.8 s boot → ~8–12 s; softens P3's eviction-storm failure mode). zlib already in kos-ports; decompress scratch ≤ chunk size. Gate: measured SH-4 inflate throughput must beat 500 KB/s comfortably — benchmark before adopting. Kill switch: per-chunk flag bit, tool emits raw on request |

## 3. Contingent dividends — real bytes, wrong time

- **T3 audio stage-B dividend, ~450–650 KB of `.bss`.** When AICA hardware
  voices land (already the plan of record — `kb/audio-plan.md` §9 says plan on
  stage B being *required*), `audiomemory`/`seq`/waveload/`CALLSTACK`/
  `pc_task_buf` shrink to sequencer-state size. Do **not** bank before stage B:
  `CALLSTACK`/`pc_task_buf` are live today (refuted once already).
- **Famicom/NES subsystem sizing (new measurement, then a user call).** The
  in-game playable NES titles drag `famicom.arc` (1.7 MB disc), `famicom_mount_archive`
  at boot, `nintendo_hi_0` (39,168 B `.bss`, 12,896 B pure slack vs the real
  `0x66a0` size printed at `boot.c:326-327`), and an unsized tree under
  `src/static/Famicom/`. Measure its `.bss`/`.data`/pool footprint with the
  same `nm -S` sweep used for L8. If material, dropping NES from the DC
  edition is an L5-class *product* decision — pre-sanctioned in spirit by
  PLAN §1's documented-cuts fallback, but the user decides, not engineering.
- **L5 offline asset decimation** — the only lever that shrinks destinations
  themselves. 640×480 target; `src/data/model` alone is 5.68 MB. User's call.
- **L6 dedup** — keep-do-not-schedule verdict stands; worth ~120 KB durable
  only if P8 never lands (they are mutually exclusive).

## 4. Experiment queue — cheapest first, each unblocks a §1 row

| # | experiment | cost | unblocks |
|---|---|---|---|
| 1 | grep top-20 `*_tex` consumers (T1 gate) | ~1 h | P2 |
| 2 | build + run `harness/dc/bench/bench_mem.c` in Flycast | ~½ day | P4, and the project's missing VRAM-read number |
| 3 | reconcile 769 vs 768 `_pc_load_src_*` | ~1 h | P1 safety argument |
| 4 | `__WriteBuffer` caller liveness | 10 min | T6 |
| 5 | Famicom tree `nm -S` sweep | ~1 h | §3 famicom decision |
| 6 | SH-4 zlib inflate throughput bench | ~½ day | pack-chunk compression |
| 7 | bucket 6 high-water (`kb/research-budget-premises.md` §2.4, on the Anbernic build, late-game save) | 1 build + playthrough | final arena sizing; **defer until within ~1 MB of fitting** (L7 verdict) — but it is the margin's largest claimant, so schedule it the moment S4 boots |

## 5. Closed — do not resurface in this plan's name

Per `kb/closed.md` and `kb/levers.md`: `-O1/-O2/-Os`/LTO/`-mrelax` (user
directive), MMU demand paging (DEAD — backing store costs ~4,000× the fault),
AICA RAM as general storage / C arrays (DMA-only over 16-bit G2; it remains a
*destination* for converted audio samples under stage B), `--icf` (no SH
backend), `-g0`/strip/compress-the-binary (0 bytes — `.bss` is `NOBITS`),
flipping `-DTARGET_PC` (non-`TARGET_PC` branch cannot even build), L4 `.text`
overlays (not needed — revisit only if §1's measured rows erode), and
re-shrinking `audiomemory` below `0x76000` (voice-dropping cliff).

## 6. Failure-mode register

| move | failure mode | detector |
|---|---|---|
| P1 | missed relocation → garbage asset or wild pointer | `packverify` + poison-fill debug build; `crash.sh` |
| P2 | CPU code reading texture bytes (no MMU ⇒ silent) | poison pattern + `dc_gx` assert; famicom special case |
| P3 | fragmentation / eviction storm on scene load | heap free/largest-block log + eviction counter |
| P4 | VRAM read too slow, or VRAM contention with textures | bench_mem numbers; window size capped by measured budget |
| P5 | something on DC actually requests a GBA payload | loader logs-and-zero-fills, never silent |
| P6/P7 | scratch-tree rewrite diverges from `src/` | byte-identical revert via `DC_SRC_SHRINK=0` |
| P8 | DL body evicted while emu64 mid-walk | pin/unpin in pool API (P3 interface requirement) |
| T5 | "boot-only" function called late | illegal-opcode guard build |

---

*Sources beyond the kb: dca3 (GTA3 DC) ships on stock 16 MB with VQ textures
resident only in VRAM — precedent for P2. No citable SH-4-from-VRAM read
bandwidth exists in public sources as of 2026-08; experiment 2 produces ours.*
