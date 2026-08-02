# Creative RAM concepts — the ideas nobody had listed

Produced 2026-08-01 by a dedicated creative pass, after six agents had already
re-costed every known lever. Its brief was explicitly *not* to re-measure
`kb/levers.md` — only to invent mechanisms absent from it.

⚠️ **These are CONCEPTS, not measurements.** Each carries its own confidence and
its own cheapest-experiment line. Nothing here is banked. Treat every number as
a claim until the named experiment runs. Read `kb/closed.md` before acting on
any of them.

The one rule they all had to satisfy: **say which side of the inequality the
idea moves.** Moving bytes from `.bss` to a heap that sits above `_end` on an
`sbrk` machine saves nothing, and that error has already cost this project two
wrong numbers.

---

## T1. Texture assets are VRAM-resident and never pooled ⭐

**This is the strongest idea in the set, and it attacks the binding constraint
directly.**

`sh-elf-nm -S` over the clean non-stub ELF decomposes the 8.5 MB of asset
destination `.bss` as:

| class | bytes | symbols |
|---|---:|---:|
| textures (`*tex*`) | **4,629,656** | 10,035 |
| `Vtx` arrays | ~3,021,392 | |
| palettes / TLUTs | 97,044 | 2,347 |
| everything else | ~0.8 MB | |

Overcount noise is ~100–200 KB (`texture_buffer_data` 49,152 and
`FONT_nes_tex_font1` 24,576 are not pack destinations), so call it
**~4.4–4.6 MB of textures**.

**The majority of L1 — "THE lever" — is data whose end consumer is not the SH-4
at all.** On Dreamcast those bytes must reach VRAM anyway. The current S4 design
would pay for them **twice**: once as a pool allocation in additive heap, again
as the VRAM copy.

The seam already exists: `dc/src/dc_gx.c:2052`
`dc_gx_backend_texture_upload(data, w, h, fmt, …)` is the declared conversion
point. Have the loader fetch a texture chunk from `assets.pak` into a transient
scratch (≤ `max_chunk_bytes` = 137,856 B), twiddle/VQ it, upload, record
`dest_addr → PVR handle`, free the scratch. The 8-byte trampoline stub is
already the L3 mechanism, so every `extern` keeps working; `dc_gx` resolves the
pointer to a handle instead of reading bytes.

**Effect: roughly halves the S4 pool requirement.** `kb/STATE.md` calls the pool
the binding constraint, so this may matter more than any `.bss` figure in the
ledger. What remains to be pooled is dominated by the 3,021,392 B of `Vtx`
arrays, which SH-4 T&L genuinely must read per frame. Per-scene residency of
*that* is plausibly ≤ 1–1.5 MB — and the N64 original ran this game in 4 MB
total, which is the existence proof that the true resident working set is small.

**Failure mode:** CPU code that reads texture bytes for non-texture purposes.
Known candidate: `src/static/Famicom/famicom.cpp:2097` reuses `nintendo_hi_0`.
The design-pattern editor works on `common_data`/save, not asset destinations —
*verify that*. With no MMU, a silent wrong read does not fault.

**Detector:** fill each stubbed texture array with a poison pattern in a debug
build and assert `dc_gx` never receives poison as pixel data.

**Cheapest experiment (~1 h):** grep `src/` for references to the 20 largest
`*_tex` symbols that are *not* `GXInitTexObj` / `gsDPSetTextureImage`. If the
count is ~0, the concept is alive.

**Confidence:** high that the relief is real; medium on mechanism cost.

## T2. The ARAM graph window lives in VRAM — additive heap −1,048,576

`dc/src/dc_aram.c` is the single seam, and every touch of the window is a bulk
`memcpy` inside `ARStartDMA` (lines 181–185) — exactly the "specific buffer with
DMA-like access" that `kb/closed.md` says second-tier memory is still legal for.

Replace `dc_mem_alloc(DCMEM_ARAM_GRAPH, …)` at `dc_aram.c:71` with
`pvr_mem_malloc(window_size)` and the two memcpys with an aligned VRAM copy
(VRAM dislikes 8-bit access; copy 32-bit-aligned, SQ-assisted on the write
side). Offsets, anchoring and zero-fill behaviour all sit above the seam and do
not change.

**−1,048,576 B of additive heap**, which includes the 536,576 B the window
doubling self-inflicted on 2026-08-01 (`kb/STATE.md` records that as debt).
Bonus: in VRAM the window can *grow* to 2–4 MB at zero main-RAM cost, holding
most of both archives' graph half and making PLAN §3.1's disc-LRU nearly
trivial. That matters — the current bring-up anchor already thrashes: mounting
`forest_2nd.arc` (4,130,656 B) rebases the window four times.

**Failure mode:** VRAM CPU-read bandwidth, the one number this project admits it
does not have. Archive reads happen at mount/scene-load time (851,744 B once at
boot; ~100–300 KB per acre), so even a pessimistic 10–15 MB/s read rate is
10–30 ms inside a load transition, not a frame cost. Second failure: VRAM
contention — 8 MB minus ~1.2 MB framebuffers minus TA buffers leaves ~4–5 MB, so
a 1 MB window fits but a 4 MB one needs T1's texture budget measured first.

**Cheapest experiment:** compile and run `harness/dc/bench/bench_mem.c`. **It
already exists, probes every main-RAM↔VRAM path with checksums, and has never
been run.** That single run also settles the open VRAM-read number for the whole
project.

**Confidence:** medium-high.

## T3. The audio stage-B dividend — ~450–650 KB nobody has credited

`kb/audio-plan.md` §9 concludes AICA hardware voices are effectively *required*
(software synthesis costs ~34% CPU even trimmed). The RAM ledger never banks
what stage B **releases**. Measured from the ELF:

```
audiomemory 589,824 + seq 275,456 + dvd_buf.3 65,536
  + CH_BUF 24,576 + CALLSTACK/pc_task_buf 58,368   =  1,013,760 B
```

Post-stage-B need ≈ 250–350 KB (largest sequence 120,192 B, bank metadata
≤ 20,736 B per font, plus sequencer state). L3 has claimed −106,496 and L8
−58,368 of this. The **unclaimed remainder is ~450–650 KB of image-side `.bss`**.

⚠️ Note the overlap with a REFUTED item: the S3 implementation pass proved
`CALLSTACK` and `pc_task_buf` are **live** today (see `kb/levers.md` L8), so
their 58,368 B is only available *after* stage B removes their consumers, not
before.

**Failure mode:** audio heap OOM deep in a scene — the jaudio heaps are carved
by `__Nas_MemoryReconfig`, and shrinking below a real peak fails at the next
`Nas_PreLoadSeq`. **Detector:** the `AUDIOHEAP SET ADDR` line already prints;
add a high-water log on the Nas heap. **Experiment:** instrument the Anbernic
build's jaudio peaks over a K.K. Saturday + fishing tournament.

**Confidence:** medium — contingent on stage B, but stage B is already the plan
of record. The risk is *when*, not *whether*.

## T4. The pool is an evictable cache inside the arena, not a second extent

The plan carries two separately-budgeted heaps: the game arena (2,705,504 B,
true peak unknown — L7) and the future S4 pool (~498 KB ceiling). Both must
individually carry worst-case margin, and **margins in separate pools
double-count**.

Every pool byte is pack-backed and therefore reloadable from `/cd` at any time,
so pool contents are *evictable by construction*. Allocate the pool from the
arena's tail and register an eviction callback on game-heap allocation failure.
The game's unknown peak and the pool then share one slack: if bucket 6's true
peak is low the pool gets the difference automatically; if high, the pool gives
it back. The additive-heap line loses the pool entirely.

**This de-risks L7's unknown instead of waiting to measure it.**

**Failure mode:** fragmentation — game allocations interleaving with pool blocks
defeat JKRExpHeap coalescing, and an eviction storm during a scene load becomes
a disc stall. **Detector:** JKRHeap free-size / largest-block logging per scene
transition, plus an eviction counter.

**This is a decision to take before S4 exists**, because it changes the loader's
allocator interface. Bytes unmeasured by construction.

**Confidence:** medium; speculative, but aimed straight at the stated binding
constraint.

## T5. Boot-tail reclaim — donate init-only `.text` to the pool

Classic embedded trick, absent from the ledger: code that runs once before the
game loop is RAM that can be reused after. `-ffunction-sections` is already on,
so a linker-script wildcard (`*(.text._pc_load_src_*)` etc.) placed at the image
end costs zero source changes; after boot, `dc/src` donates
`[__init_reclaim_start, _end_of_image)` to the pool.

Measured floor: `_pc_load_src_*` = **87,736 B** (768 functions), `pc_assets.c`
remaining text 10,088, one-shot `*bswap*` passes 4,824 → **102,648 B measured**,
plausibly 150–300 KB with curated one-shot init (`sound_initial`,
`initial_menu_init`, hardware init).

Moves the **image-span** side *and* hands the bytes to the pool — dual credit.

**Failure mode:** a "boot-only" function called later jumps into pool data.
Silent and catastrophic. **Detector:** a guard build fills the donated region
with SH-4 illegal-instruction opcodes instead of donating it; any late call
faults loudly and `crash.sh` symbolises it. **Caveat:** under S4 the loader
functions may become lazy entry points — resolve which are truly boot-only from
`assets_scan.py`'s replay before curating.

**Confidence:** medium-high on the floor, medium on the upside.

## T6. `dvd_buf.3` — a 65,536 B census miss

`src/static/jaudio_NES/internal/dvdthread.c:105`, `static u8 dvd_buf[0x10000]`
(double 32 KB DVD bounce, `__WriteBufferSize(dvd_buf, 2, 0x8000)`). `kb/levers.md`
L8 counted `CALLSTACK` (line 29) and `pc_task_buf` from the same subsystem but
**missed this, and it is bigger than both combined**.

Even if the bounce is live for bank loads it can shrink to one 8–16 KB buffer.
**−49 KB to −65 KB of image-side `.bss`**, high confidence, needs a 10-minute
liveness check of `__WriteBuffer` callers. Given that the same brief's claims
about `CALLSTACK` and `pc_task_buf` were both refuted on inspection, **do that
check before believing this one.**

## T7. Trim the soft additive reserves — ~80–110 KB

`threads 65,536` reserves two future app-thread stacks and the port creates
none; KOS `THD_STACK_SIZE` is 32,768, so halve it to one. `KOS 262,144` is
itself a round-up of a measured 216,704 that already includes a "future"
`snd_stream` buffer. Both are ledger constants in `dc/` headers. Moves additive
heap, near-zero risk, detector is the existing `MEMLEDGER FIT` line. Small but
free.

## T8. OC-RAM — 8 KB of SH-4 operand-cache-as-RAM at `0x7c001000`

KOS's linker script already carries `.ocram (NOLOAD)` (visible as section 12 in
`readelf -S`). Put rspsim's 4 KB `DMEM[0x1000]` there — it is the hottest audio
buffer and the only genuinely "emulated memory" in the build — or a thread
stack. −4 to −8 KB of `.bss` plus a speed win. **Halves the operand cache;
measure before shipping.** Kill switch trivial. Listed only because it is nearly
free.

## T9. Overlap check, not a claim — `l_keepSave` staging

**Now resolved: this WAS inside L3's `pc_m_card` row and has been banked** (see
`kb/levers.md`). Kept here only to record the alternative the creative pass
raised, in case the banked form ever has to be reverted: stream the VMU
chunked-deflate writer directly from live `common_data` per `kb/save-budget.md`
§4.3's 16 KB chunks, eliminating the staging copy entirely.

---

## Doc errors this pass found

1. **`kb/levers.md` L8 missed `dvd_buf.3` = 65,536 B** — in the very file whose
   lines 26/29 it cites, and larger than both items it lists from that
   subsystem. Now recorded as T6.
2. **`kb/levers.md`'s header was stale against `kb/STATE.md`** — it stated the
   gap as 8,273,108 and the `.bss` ceiling as 4,143,556, both pre-dating the
   ARAM window doubling. Anyone costing from `levers.md` alone started 537 KB
   optimistic. Fixed.
3. **`kb/STATE.md` quoted two `.bss` figures without labelling which link each
   described** (12,415,796 headline vs 12,415,508 in the boot-status table).
   Both were real, from different links. Fixed.
4. The ELF contains **768** `_pc_load_src_*` symbols; `kb/asset-pack.md` says
   **769** loader functions. One got `--gc-sections`'d or lives under another
   name. **Reconcile before S4 trusts the replay's completeness** — the pack
   tool's whole safety argument rests on that count.
5. The destination-`.bss` decomposition in T1 (tex 4.63 MB / `Vtx` 3.02 MB /
   pal 0.10 MB / rest 0.8 MB) appears in no other document, and it is the single
   most useful fact for planning S4.
