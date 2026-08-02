# The heap is TWO pools that compete

Read this before changing `DC_ARENA_BYTES`, `DC_ARAM_WINDOW`, or anything that
allocates at boot. Getting the direction wrong here cost a full debug cycle and
produced a run that got *less* far. Live measurements of what the pools
actually hold are in `kb/state-log.md`; the ranked size levers are in
`kb/levers.md`.

## ⚠️ THE HEAP IS TWO POOLS THAT COMPETE, AND THIS WAS BEING GOT BACKWARDS

Everything between the end of `.bss` and `_arch_mem_top` (`0x8d000000`) is
shared by **two** allocators:

| pool | who uses it |
|---|---|
| the arena (`DC_MAIN_MEMORY_SIZE`, bucket 6) | `__osMalloc` / `zelda_malloc` / JKRHeap |
| KOS `sbrk` → libc `malloc()` | `graph_proc`'s `malloc(alloc_size)`, the scene loaders |

The arena is carved out of that region with `memalign`, so **every byte given to
the arena is a byte libc can never hand out.**

Measured, this session:

- With arena = 2,705,504 the title-demo scene died with
  `Out of memory. Requested sbrk_base 8d0ee000, was 8cec5000, diff 2265088`.
  That is **KOS's sbrk**, not the arena — `8d0ee000` is past the top of RAM.
  libc had 1,290,240 B left and wanted 2,265,088.
- Raising the arena to 4,980,736 to "fix" it made the run get **less** far
  (stopped at `trademark_init` instead of reaching `play_main`), because the
  extra 2.27 MB came straight out of libc's share.
- **Lowering** the arena to 1,900,000 and the ARAM window to 851,968 removed
  the OOM entirely and took submitted geometry from **46 triangles to 557,971**.

**Rule: when the sbrk OOM fires, shrink the arena, the ARAM window, or the
image — never grow the arena.** Bucket 6's own high-water is still unmeasured;
no arena-side OOM has ever been observed, which is weak evidence that 2,705,504
is generous, not proof. New knobs: `DC_ARENA_BYTES`, `DC_ARAM_WINDOW`,
`DC_DIAG`, `DC_FB_PROBE` (all in `dc/Makefile`, forwarded by `dc/build-dc.sh`).

⚠️ ~~**851,968 is a floor for the ARAM window, not a preference:**
`forest_1st.arc` arrives as one 851,744 B transfer, and a window smaller than
that drops the whole archive on the floor.~~
**SUPERSEDED 2026-08-02 by PLAN §3.1's disc-backed pager, and the premise was
wrong anyway.** `forest_1st.arc` does NOT arrive as one 851,744 B transfer: it
arrives as 26 transfers of ≤32,768 B (`JKRAramStream::writeToAram`, 0x8000
chunks) covering one *contiguous* 851,744 B ARAM extent. The floor was about
the old window having to be one contiguous span, not about any single DMA.

With `DC_ARAM_LRU=1` the window is no longer a window — it is a block cache in
front of an extent map that re-reads archive ranges from `/cd`, so it does not
have to hold anything. **The measured setting is `DC_ARAM_WINDOW=131072`**
(4 × 32 KB blocks): a full title-screen run maps all 4,982,400 B of graph-half
writes, loses 0 B, zero-fills 0 B, and never pins a block. Read the
`[DC/ARAM] LRU …` console line before changing it — `LOST=` must be 0 and
`pin_peak=` is what sizes the pool. The old floor still applies with
`DC_ARAM_LRU=0`.

