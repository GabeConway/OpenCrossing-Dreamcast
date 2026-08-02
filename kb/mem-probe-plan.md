# Memory high-water probe plan — instrumenting the PC build (was mem-budget §6)

The unexecuted patch plan for measuring the real `__osMalloc` / `JKRExpHeap`
peak, the graph-ARAM working set, and per-acre asset residency on the cheap
armhf/desktop build (`PC_MEMPROBE=1`). Read when the arena's true high-water
mark blocks a decision — it is still the #1 open measurement.
Cross-refs: `kb/mem-budget-void-ledger.md` §4 (bucket 6 / §7 question 1),
`kb/research-budget-premises.md` §2.4.

## 6. Patch plan: instrument the **existing PC build** for real high-water
marks (task 6)

Goal: replace bucket 6's `[?]` with a measured number, and size the bucket 8
window from data. All of this runs on the armhf/desktop build where it is
cheap. Nothing here is Dreamcast-specific.

### New module

`pc/src/pc_memprobe.c` + `pc/include/pc_memprobe.h`, compiled only when
`-DPC_MEMPROBE=1` (add the option to `pc/CMakeLists.txt`; keep it off by
default so the shipping build is unchanged — same kill-switch discipline as
the `PC_NO_*` env vars).

State: a fixed array of probes, each `{name, cur, peak, peak_tag, peak_frame}`.
Two entry points: `pc_memprobe_sample(void)` (cheap, per frame) and
`pc_memprobe_tag(const char*)` (scene label).

### Probe 1 — game malloc arena (the number that sets bucket 6)

`src/static/libc64/malloc.c` already exports exactly what we need:

```c
extern void GetFreeArena(size_t* max, size_t* free, size_t* alloc);  /* TARGET_PC variant, line 19 */
```

`alloc` is used bytes; `max` is the largest free block (fragmentation).
No game-code change needed to *read* it.

For an exact high-water (not a per-frame sample, which can miss transient
spikes during a scene load), add to `src/static/libc64/__osMalloc.c`, guarded
by `#ifdef PC_MEMPROBE`:

- in `__osMalloc` (line 265) and `__osMallocR` (line 269): after a successful
  block split, `g_probe_arena_cur += size; if (cur > peak) peak = cur, record tag`.
- in `__osFree` (line 381): `g_probe_arena_cur -= size`.

This is ~8 lines in two functions and is the highest-value patch in this
document.

### Probe 2 — JKRHeap system heap

`JKRHeap::getTotalFreeSize()` / `getFreeSize()` on
`JFWSystem::getSystemHeap()` and on the root heap; used = heap size − total
free. Sample per frame via the probe module. For exact peaks, add the same
`#ifdef PC_MEMPROBE` counter pair to `JKRExpHeap::do_alloc` / `do_free`
(`src/static/JSystem/JKernel/JKRExpHeap.cpp`).

Also record `getFreeSize()` (largest contiguous) separately — the gap between
"total free" and "largest free" *is* the fragmentation number that §4.2 warns
about, and it is what decides the safety margin on top of the measured peak.

### Probe 3 — graph-ARAM working set (sets bucket 8)

The highest-value measurement after probe 1. In `pc/src/pc_aram.c`, inside
`ARStartDMA` (the memcpy seam), record every (aram_addr, length) into a
bitmap at 2 KB page granularity. Maintain rolling unique-page counts over
1 s / 5 s / 30 s windows plus an all-time max. Dump at exit.

Output: "the largest 5-second graph-ARAM working set over a full playthrough
was N KB" → bucket 8 = N + 50 %. Today's 512,000 B is a placeholder.

Do the same for the sound half to confirm the AICA-side budget.

### Probe 4 — renderer and asset numbers (cheap, confirms §4.1)

- `g_gx.vertex_count` peak per frame → validates shrinking
  `PC_GX_MAX_VERTS` from 65536 to 8192. If the real peak is >8192, bucket 5's
  arithmetic changes and we need to know now.
- `tex_cache_count` (`pc/src/pc_gx_texture.c:96`) peak and total decoded
  bytes → sizes the VRAM texture budget.
- Number of *distinct* asset groups touched per acre/room → sizes bucket 7
  (the 1.5 MB asset pool). Instrument by giving each of the 2,080 asset
  objects an ID in `gen_runtime_assets.py` and logging first-touch. This is
  the measurement that decides whether C6 is 1.5 MB or 3 MB.

### Hook points

| File | Where | Call |
|---|---|---|
| `src/static/jsyswrap.cpp` | end of `JW_Init2` (after `MallocInit`, line 549) | `pc_memprobe_init()` — captures heap base/size |
| `pc/src/pc_vi.c` | `VIWaitForRetrace` | `pc_memprobe_sample()` |
| `src/game/m_play.c` / `src/game/m_field_info.c` | scene/acre transition | `pc_memprobe_tag("town:acre_N")` etc. |
| `pc/src/pc_main.c` | after `boot_main` returns, and in the crash handler | `pc_memprobe_report()` |

Put the report in the crash handler too — the peak that matters is often the
one immediately before an OOM.

### Output and reduction

- `mem_profile.csv`, one row per sampled frame:
  `frame,tag,sysheap_used,sysheap_peak,sysheap_largest_free,arena_used,arena_peak,arena_largest_free,aram_sound_used,aram_graph_ws_1s,aram_graph_ws_5s,gx_verts,texcache_bytes,texcache_count`
- `pc/tools/mem_report.py` (new, host-side) reduces it to a per-tag peak table
  and prints the proposed `sysHeapSize`/`gameheap` values directly, so the
  output of the measurement is the patch.
- Optional `PC_MEMPROBE_SITES=1`: record `__builtin_return_address(0)` with
  each `__osMalloc`, dump a top-50 allocation-site histogram, resolve with
  `addr2line`. This is what tells us *which* subsystem to attack if the peak
  comes in over 4 MB.

### Driving it

`harness/smoke.sh armhf` already boots with the real ROM. Add
`PC_MEMPROBE=1` to a new `harness/mem_profile.sh` that runs the existing
playthrough script (kb/build-test.md) end to end and then runs
`mem_report.py`. Worst case must come from a **late-game save with a full
house and a full town**, not a fresh boot — that is the number bucket 6 has
to survive.

---
