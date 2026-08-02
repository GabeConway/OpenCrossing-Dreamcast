# Second-tier memory (VRAM / AICA) — SALVAGED FRAGMENT

**Status: the agent investigating this died before writing its document.**
Everything below was recovered from its scratchpad by the main thread on
2026-08-01. Treat it as raw material, not as a finished research doc.

## What was recovered

1. **A complete, unrun benchmark.** `harness/dc/bench/bench_mem.c` (433 lines),
   rescued into the repo. It probes every path by which the SH-4 can move a
   block between main RAM and:
   - PVR VRAM via the 32-bit window, the 64-bit window, store queues, PVR DMA
     (ch2) and the SH-4 on-chip DMAC (ch3) — **both directions**;
   - AICA sound RAM via G2 PIO and G2 DMA — **both directions**.

   Sizes 4,096 / 65,536 / 262,144 B, 8 reps, best-of reported, and — good
   discipline — **every result checksum-verified before being reported**, on
   the principle that a fast number from a transfer that did not actually move
   the bytes is worse than no number.

   Build/run (it is host tooling, so the `-O0` directive on `src/` does not
   apply):
   ```
   kos-cc -O2 -o bench_mem.elf harness/dc/bench/bench_mem.c
   # then wrap in a CDI and run under harness/dc/smoke.sh; output goes to scif
   ```

   ⚠️ **It was never compiled and never run.** There are no measured numbers
   from it. It has not even been checked for compile errors against KOS 2.3.

2. **Community bandwidth figures, gathered but not attributed.** The agent had
   collected these from a Dreamcast developer discussion; **the URL was lost
   with the agent, so these are uncited and must be re-sourced before being
   relied on.** Recording them because they are useful priors:
   - CPU↔BSC bus 200 MHz × 4 B = 800 MB/s; main RAM bus 100 MHz × 8 B =
     800 MB/s — no bottleneck between them.
   - **Store queue → main RAM measured at 495 MB/s**, which exceeds the
     400 MB/s a 32-bit-only path would cap at, so the SH-4 does recombine into
     full 64-bit bus width.
   - Cache path (`MOVCA`/`OCBWB`) ≈ 493 MB/s — essentially the same.
   - **Read-then-write-back of a cacheline: 223.5 MB/s best case** (reads
     cached, writes via SQ); cache for both drops to 185 MB/s. Predicted
     246 MB/s from 13 bus cycles per 32 B.
   - SQ → TA ≈ 200 MB/s vs DMA ≈ 9 MB/s in one poly benchmark, though the
     author flagged that run as having had a copy-paste bug.
   - Pedantry worth preserving: the author's figures are MiB, not marketing MB.

   **None of these is a VRAM *read* figure**, which was the specific gap
   `kb/research-size-reduction.md` flagged as UNVERIFIED. That gap is still
   open.

## What this fragment does NOT establish

- SH-4↔VRAM read bandwidth — **the original question, still unanswered.**
- How much VRAM is genuinely spare after framebuffer + tile accumulation +
  a realistic twiddled/VQ texture working set.
- Whether a non-texture blob survives untouched in VRAM across PVR scene
  operations.
- Real achievable G2/AICA throughput and per-transfer latency, and whether
  audio's needs conflict with using AICA RAM as a store.
- Cache flush/invalidate cost around block moves.
- Any precedent for a shipped DC project using VRAM or AICA as general storage.

## How much this now matters

**Less than when it was commissioned.** `kb/research-mmu-paging.md` returned
**DEAD** on MMU demand paging, and its central argument generalises: the
bottleneck is never the fault mechanism, it is the backing store. Second-tier
memory was primarily interesting as a *fast backing store for paging*. With
paging dead, the remaining uses are narrower:

- VRAM as a destination for `texture_buffer_data` and decoded textures, so
  they never occupy main RAM (already partly taken — see `kb/STATE.md`).
- AICA RAM holding audio samples so `audiomemory`/jaudio buffers leave main RAM
  (~0.65 MB on the ranked list; see `kb/audio-plan.md`).

Both are *targeted relocations of specific buffers*, not general-purpose
storage, and neither needs the full bandwidth study to proceed. **Re-running
this investigation is low priority.** If you do resume it, the benchmark is
already written — build it, run it, and record real numbers.
