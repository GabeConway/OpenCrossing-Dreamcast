# Budget premises §2 — bucket 6, the 4,000,000 B arena (and §2.4, how to measure it)

§2 of `kb/research-budget-premises.md`, moved verbatim, including **§2.4, the
bucket-6 measurement recipe** that `kb/levers.md` and `kb/ram-plan.md` cite by
name, and **§2.2**, the 1,294,497 B of provably dead XFB + GX FIFO.
Read before sizing the arena. **Status: NOT resolved — bucket 6 could be right
at 4 MB or could need 5.5 MB.**

> ⚠️ **[2026-08-06] the arena analysis below is unaffected by the `-O0`
> reversal, but the budget it sits inside is not.** `src/` now builds at `-Os`
> with a 14-TU `-O3` hot list (`DC_OPT_PROFILE=perf`); `dc/src` is `-O3`.
> Measured on the shipping town build: `.text` **5,506,964 → 2,753,700 B**
> (2,680,676 at flat `-Os`), `.data` **2,337,980 → 2,224,832 B**, `.bss`
> unchanged (3,945,356 → 3,945,484). Codegen was worth ~2.75 MB of `.text`.
> Nothing in §2 changes — the XFB and FIFO are still dead, the 1,294,497 B is
> still recoverable, the `__osMalloc` peak is still unmeasured, and §2.4 is
> still the recipe. What changes is the *pressure*: image bytes and heap bytes
> come out of one pool, so a smaller `.text` is directly more arena headroom,
> and "bucket 6 might need 5.5 MB" is a less frightening sentence than it was.
> Do not treat that as permission to skip §2.4. Evidence: the 2026-08-06 entry
> of `kb/state-log.md`.

---

## 2. Bucket 6 — the 4,000,000 B arena (the highest-value unknown)

**Status: NOT resolved. Partly characterised. Read §2.4 for the recipe.**

### 2.1 What the arena is, and what it is not

`DC_MAIN_MEMORY_SIZE = 4,000,000` (`dc/include/dc_platform.h:102`) is
`#error`-enforced equal to `DC_BUDGET_JKRHEAP` (`dc/src/dc_os.c:24-25`), so
**bucket 6 and the arena are the same object**, not two costs. It is obtained
once at `dc/src/dc_os.c:400` via `dc_mem_alloc(DCMEM_JKRHEAP, …)` and
`memset` to zero at `:413`. It is the emulated GameCube MEM1: every
`OSPhysicalToCached`, every `JKRHeap`, and `__osMalloc` live inside it.

`kb/STATE.md`'s note that it is "4 MB *on top of* the 22.5 MB image" is
correct but easy to misread — it is on top of the image *and already inside
the ledger*, not an extra unbudgeted 4 MB.

**Where 4,000,000 came from: nobody measured it.** It is a round number chosen
to fit the ledger. The evidence is in the header comment itself
(`dc_platform.h:100-103`): the size was picked to satisfy the bucket, and the
bucket was picked to make §4 of `kb/mem-budget.md` sum under 16 MB. The
GameCube/PC value it replaced was 25,165,824 B.

### 2.2 Two allocations inside the arena are provably dead on Dreamcast — 1,294,497 B (32 % of bucket 6) [M]/[D]

**This is the concrete result of this pass and it needs no measurement.**

**(a) The external framebuffers: 1,228,800 B.**
`src/static/jsyswrap.cpp:507` calls
`JC_JFWDisplay_createManager_0(&GXNtsc480IntDf, systemHeap, 2, 1)`.
`2` is `JUTXfb::DoubleBuffer` (`include/JSystem/JUtility/JUTXfb.h:21`).
That reaches `JFWDisplay.cpp:60` → `JUTXfb::createManager` →
`JUTXfb::initiate(w=640, h=480, systemHeap, DoubleBuffer)`
(`src/static/JSystem/JUtility/JUTXfb.cpp:63-77`):

```c
u32 size = (u16)ALIGN_NEXT((u16)w, 16) * h;      /* 640 * 480 = 307,200 */
mBuffer[0] = new (heap, 32) u16[size];           /* 614,400 B */
if (number >= DoubleBuffer) mBuffer[1] = new (heap, 32) u16[size];
```

`GXNtsc480IntDf` is `{0, 640, 480, 480, …}` (`src/static/dolphin/gx/GXFrameBuf.c:31-33`),
so `fbWidth=640`, `xfbHeight=480`. **2 × 614,400 = 1,228,800 B from the
system heap.** The file contains no `#if` at all — it is compiled identically
for GameCube, PC and Dreamcast.

**They are never read on Dreamcast.** Every consumer of the XFB pointers
terminates at `VISetNextFrameBuffer` (`JFWDisplay.cpp:381`,
`JUTVideo.cpp:106`, `JUTVideo.cpp:150`), and `dc/src/dc_vi.c:38` is
`void VISetNextFrameBuffer(void* fb) { (void)fb; }` — a no-op. The PVR owns
the real framebuffer, in VRAM. So 1,228,800 B is allocated, zeroed, and
never touched.

*Caveat for whoever implements the kill:* `JUTVideo.cpp:149` guards on
`if (xfb->getDrawnXfb())`, so a `TARGET_DC` branch must hand back a **non-NULL
dummy pointer**, not NULL, or the display path changes behaviour.

**(b) The GX FIFO: 65,697 B.**
`jsyswrap.cpp:493` sets `fifoBufSize = 0x10001`; `JFWSystem.cpp:131` calls
`JUTCreateFifo`, and `JUTGraphFifo::JUTGraphFifo` (`JUTGraphFifo.cpp:9-24`)
does `JKRAllocFromSysHeap(mSize + 0xA0, 32)` = 65,697 B. `dc_platform.h:115`
already documents this as "a JKRHeap-allocated FIFO buffer we do not need",
but **nothing gives it back** — `DC_FIFO_SIZE` has zero references in
`dc/src/dc_gx.c`.

**Together: 1,294,497 B, 32.4 % of bucket 6, recoverable with two `TARGET_DC`
branches and no measurement.**

### 2.3 What is left, and why the peak is still unknown

Other arena consumers found (all [S]):

| consumer | bytes | file:line |
|---|---:|---|
| XFB double buffer | 1,228,800 | `JUTXfb.cpp:70-74` — **dead, §2.2** |
| GX FIFO | 65,697 | `JUTGraphFifo.cpp:18` — **dead, §2.2** |
| `JUTConsole::create(60, 200)` | ~12,000 [?] | `JFWSystem.cpp:140` |
| `JUTException` console buffer | 9,464 | `JFWSystem.cpp:155`, `exConsoleBufferSize = 0x24F8` |
| RARC directory tables, both archives | **2,976** | **[M]** — measured from the real ISO, §3.5 |
| `JKRDvdRipper`/`JKRAram` Yaz0 scratch | 1,024 + 4,384 | `JKRDvdRipper.h:10-11` |
| `mainThread`, `JKRThread`, fader, video mgr | small | `JFWSystem.cpp:129-136` |
| **`gameheap` → `MallocInit` → `__osMalloc`** | **all the rest** | `jsyswrap.cpp:547-549` |

The decisive line is `src/static/jsyswrap.cpp:547-549`:

```c
gameheap_len  = JC_JKRHeap_getFreeSize(JC_JFWSystem_getSystemHeap()) - 0x10000;
gameheap_base = JC_JKRHeap_alloc(JC_JFWSystem_getSystemHeap(), gameheap_len, 32);
MallocInit(gameheap_base, gameheap_len);
```

**The game heap takes whatever is left, so it never fails to allocate and
never reports a shortfall.** That is precisely why nobody has ever measured
it — shrinking the arena silently shrinks `__osMalloc`'s pool, and the first
symptom is an out-of-memory deep in a scene load, not at boot.

With the arena at 4,000,000 B, `__osMalloc` would today receive roughly:

```
4,000,000 − 0x3100 (low reserve) − 0xD0 − 1,228,800 (XFB) − 65,697 (FIFO)
          − ~30,000 (console/exception/misc) − 0x10000 (MallocInit reserve)
        ≈ 2,585,000 B                                              [D]
```

Killing the XFB and FIFO raises that to ≈3,880,000 B **without growing the
arena**, or lets the arena drop to ~2.75 MB at today's effective pool size.

**Whether ~2.6 MB is enough for `__osMalloc` is unknown.** Two data points,
both weak, pointing in opposite directions:

- `jsyswrap.cpp:37` `gameheap_len = 0x380000` (3,670,016 B) is a **dead
  default** — never executed, but it is plausibly the value the GameCube
  original shipped with. If so, the retail game wanted ~3.5 MB, and 2.6 MB is
  **not enough**. *This is the single most important thing to confirm.*
- `jsyswrap.cpp:36` `SystemHeapSize = 0x16C7000` (23,899,136 B) is likewise
  dead and is obviously "the whole arena", carrying no information about need.

**I could not resolve this, and I am not going to guess.** The honest position
is: **bucket 6 could be right at 4 MB, or could need to be 5.5 MB.** The
optimistic reading offered in the brief — "if the real peak is 1.5 MB, that is
2.5 MB found for free" — is **not supported by anything I found**, and the
`0x380000` default is weak evidence *against* it. Do not plan on it.

### 2.4 How to measure it — the recipe

The PC/armhf reference port at
`/Users/gabe/Documents/GitHub/OpenCrossing-Anbernic` shares `src/` and runs on
the host. Instrument it there; it costs one build and one playthrough.

1. **Read the existing exported counter first — it may be enough.**
   `src/static/libc64/malloc.c` already exports
   `void GetFreeArena(size_t* max, size_t* free, size_t* alloc)` under
   `TARGET_PC` (~line 19). `alloc` is bytes in use; `max` is the largest free
   block (i.e. the fragmentation number `kb/mem-budget.md` §4.2 warns about).
   **Sampling this per frame needs no game-code change at all.** Do this
   before writing any patch.
2. **For an exact high-water** (per-frame sampling misses load-time spikes),
   add a counter pair under `#ifdef PC_MEMPROBE` in
   `src/static/libc64/__osMalloc.c`: increment in `__osMalloc`/`__osMallocR`
   after a successful split, decrement in `__osFree`, and record
   `peak`/`peak_tag`. **`kb/mem-budget.md` §6 cites lines 265/269/381 for
   these; those line numbers were NOT verified in this pass — re-derive them.**
3. Do the same for `JKRExpHeap::do_alloc`/`do_free`
   (`src/static/JSystem/JKernel/JKRExpHeap.cpp`) to separate the JKRHeap side
   from the `__osMalloc` side. They are billed to the same 4 MB.
4. **Drive it from a late-game save with a full house and a full town**, not a
   fresh boot. A fresh boot's peak is not the number bucket 6 must survive.
5. Report three numbers: `__osMalloc` peak, `JKRExpHeap` peak, and
   `largest_free / total_free` at the peak. Set bucket 6 to
   `peak × 1.15` for fragmentation, plus the §2.2 savings.

**Until step 5 reports, treat every total in §1 as provisional.**
