# Budget premises — audit of the numbers the RAM plan rests on

Written 2026-08-01. **Two of six questions are only partly answered.** §6 lists
exactly what is missing and how to finish it.

Purpose: `kb/mem-budget.md` and `kb/research-size-reduction.md` derive an
"image budget" of **8,035,072 B** and a "required cut" of **14,451,476 B**.
Several inputs to that subtraction had never been measured. This document
re-derives it from measurements.

Tags: **[M]** measured today · **[S]** read from source (KOS/SDK/decomp, cited
file:line) · **[D]** derived arithmetic · **[?]** still unverified.

---

## 1. Corrected budget — the headline

### 1.1 The claimed budget was wrong in kind, not just in magnitude

The old derivation subtracted a list of "heap buckets" from 16 MB and called
the remainder an image budget. **Three of those buckets are not heap at all —
they are `.bss` inside the image — and a fourth double-counts bytes that are
already in the image.** Subtracting them a second time understated the budget
by ~3.77 MB.

| Input | Claimed | Corrected | Tag | Why |
|---|---:|---:|---|---|
| Physical RAM | 16,777,216 | 16,777,216 | [S] | `_arch_mem_top = 0x8d000000`, `arch.h:42` |
| Low RAM below load address | −65,536 | −65,536 | [S] | image loads at `0x8c010000`; `arch.h:297` rejects anything lower |
| Kernel stack hard reserve | −65,536 | −65,536 | [S] | `mm.c:52` refuses `sbrk` past `_arch_mem_top − THD_KERNEL_STACK_SIZE`; `stack.h:52` = 64 KB |
| **= usable for image + heap** | **16,646,144** | **16,646,144** | [D] | unchanged, and correct |
| Bucket 1 KOS | −1,000,000 | **−262,144** | [M]/[S] | **double-counted.** KOS+newlib+libstdc++ are *inside* our ELF: 304,829 B measured from the map. Only ~151 KB is genuinely additive heap. §3.2 |
| Bucket 6 JKRHeap/`__osMalloc` arena | −4,000,000 | −4,000,000 | **[?]** | genuinely heap-allocated. **True need still unmeasured** — but ≥1,294,497 B of it is provably dead. §2 |
| Bucket 7 asset pool | −1,500,000 | −1,500,000 | [D] | legitimate, but *future*: today those bytes are `.bss` in the image, so counting both is double-counting until the conversion lands. §3.4 |
| Bucket 8 ARAM graph window | −512,000 | −512,000 | **[?]** | genuinely heap-allocated (`dc_aram.c:49`). Size unmeasured. |
| Bucket 9 audio 700,000 | −700,000 | **−0** | [M] | **not heap.** The buffers are static `.bss`: `jaudio_NES` = 1,265,101 B, already in the image |
| Bucket 10 disc I/O 384,000 | −384,000 | **−0** | [M] | **not heap.** `dc_dvd.c.o` `.bss` = 13,320 B, in the image. No read-ahead ring exists yet |
| Bucket 11 PVR staging 384,000 | −384,000 | **−0** | [M] | **not heap.** `g_gx` = 334,764 B of `.bss` in `dc_gx.c.o`, in the image |
| Bucket 12 stacks 131,072 | −131,072 | −65,536 | [S] | KOS's 64 KB kernel stack is already subtracted above; idle/reaper stacks are 1,024 B of `.bss`. Reserve 64 KB for future app threads |
| **= image budget** | **8,035,072** | **10,306,464** | [D] | |

### 1.2 The honest fit test

Splitting one pool into "image budget" and "heap budget" is what caused the
error. State it as a single inequality instead:

```
(image span) + (genuinely additive heap) ≤ 16,646,144        [D]
```

| | bytes | tag |
|---|---:|---|
| image span today (`0x8c010000` → `_end 0x8d472814`) | **21,374,996** | [M] |
| additive heap today (KOS 262,144 + arena 4,000,000 + ARAM window 512,000 + threads 65,536) | **4,839,680** | [M]/[?] |
| usable | 16,646,144 | [D] |
| **over by** | **9,568,532** | [D] |

Adding the future 1.5 MB asset pool (the change that also removes 8.45 MB of
`.bss`, so it cannot be counted without its own credit):

| | bytes |
|---|---:|
| image budget with asset pool reserved | **10,306,464** [D] |
| image today | 21,374,996 [M] |
| **required cut** | **11,068,532** (10.56 MiB) [D] |

### 1.3 So: is the cut 14.45 MB, 12 MB, or 16 MB?

**It is ~11.07 MB, not 14.45 MB — and about 1.1 MB of that difference is
simply that the 14.45 MB figure is stale.**

| claim | bytes | status |
|---|---:|---|
| `kb/STATE.md` "image 22,486,548, cut 14,451,476" | 14,451,476 | **stale.** The linked ELF on disk is 21,374,996 — the §8.3 fixes already landed. Overstates by 1,111,552 [M] |
| same budget, current image | 13,339,924 | arithmetically right, **budget wrong** (§1.1) |
| **corrected** | **11,068,532** | [D], contingent on bucket 6 = 4 MB [?] |
| corrected, if the dead XFB/FIFO allocations in §2.2 are removed | **9,774,035** | [D] |

**≈3.4 MB was found, and none of it came from making anything smaller.** It
came from the budget double-counting bytes. That is the good news; §1.4 is the
bad news.

### 1.4 What did NOT improve, and it is the binding constraint

The coordinator's arithmetic is correct and survives this audit:

```
.text-equivalent (text+rodata+init/fini/ctors/eh_frame)   6,318,568  [M]
.data                                                      2,638,852  [M]
                                                      = 8,957,420
image budget (corrected)                                  10,306,464  [D]
⇒ headroom for ALL of .bss                                 1,349,044  [D]
.bss today                                                12,415,508  [M]
```

So the corrected budget buys **1,349,044 B of `.bss` headroom instead of a
negative number** — the port goes from arithmetically impossible to merely
very hard. But `.bss` must still fall from 12,415,508 to ~1.35 MB, i.e.
**−89.1 %**, and with `-O0` frozen `.text` cannot help.

A useful decomposition of the 11.07 MB cut against the ranked plan in
`kb/research-size-reduction.md` §6.2:

| lever | Δ | status |
|---|---:|---|
| `src/data` `.bss` demand residency | −8,449,191 | unbuilt; **76 % of the cut** |
| `s_assets[]` string pool → disc index | −888,853 | [M] confirmed exactly, §3.3 |
| `src/data` `.data` eviction (pointer-free first) | −948,688 [?] | figure not re-verified, §6.4 |
| dead XFB + GX FIFO (heap side, §2.2) | −1,294,497 | [M]/[D] new this pass |
| **subtotal** | **−11,581,229** | vs 11,068,532 required |

It closes with a 512,697 B margin (4.6 %) — thinner than it looks, because the
largest line is unbuilt and the second-largest is unverified.

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

---

## 3. What was established, with evidence

### 3.1 KOS's memory model — confirmed exactly [S]

Read from the SDK image (`opencrossing-dc:sdk`), not from secondhand docs:

```
0x8c000000 ─ 0x8c010000   64 KB  below page_phys_base — unusable   arch.h:53,297
0x8c010000 ─ _end                the ELF: game + KOS + newlib, all of it
_end       ─ 0x8cff0000          the heap; sbrk_base = align(end,4)  mm.c:31-34
0x8cff0000 ─ 0x8d000000   64 KB  kernel thread stack, hard-reserved mm.c:52
```

- `_arch_mem_top = 0x8d000000` — `arch.h:42`
- `THD_KERNEL_STACK_SIZE = 64 * 1024` — `stack.h:52`
- `THD_STACK_SIZE = 32768` (per app thread; none created at boot) — `stack.h:47`
- `mm_sbrk` returns `ENOMEM` at `_arch_mem_top − THD_KERNEL_STACK_SIZE` — `mm.c:50-55`

**Usable for image + heap = 16,646,144 B.** The "every `.bss` byte destroys a
heap byte" claim is confirmed at the source: the heap literally begins at
`end`.

### 3.2 Bucket 1 (KOS 1,000,000) is a double-count and ~3.8× too large [M]

Measured from `dc/build/AnimalCrossing.map` by attributing every allocatable
input section to its owning archive:

| archive | allocatable bytes in our image |
|---|---:|
| `libkallisti.a` | 130,563 (`.text` 95,238 · `.bss` 20,197 · `.rodata` 9,460 · `.data` 5,668) |
| `libc.a` | 84,665 |
| `libstdc++.a` | 48,991 |
| `libgcc.a` | 27,374 |
| `libm.a` | 13,236 |
| **total** | **304,829** |

All 304,829 B are **inside** the 21,374,996 B image span. Subtracting
1,000,000 B on top of the image counts them twice.

Independently corroborated by building test programs in the SDK container:
a bare KOS hello-world's image is **230,488 B**; `+pvr_init_defaults()` adds
**764 B** and **zero heap** (the entire main-RAM PVR state is `_pvr_state`,
520 B of `.bss`; TA/OPB buffers are VRAM offsets, `pvr_buffers.c:252-310`).

Genuinely **additive** KOS heap, read from source:

| item | bytes | file:line |
|---|---:|---|
| kernel stack reservation | 65,536 | `mm.c:52` |
| `fs_iso9660` sector cache + headers | 65,792 | `fs_iso9660.c:1277-1278` |
| maple DMA buffer | 16,384 | `maple_init_shutdown.c:129` |
| 3 × `kthread_t` (kern/idle/reaper) | 3,456 | `thread.c:426,1117-1136` |
| *future* `snd_stream` sep buffer | 65,536 | `snd_stream.c:353` |
| **total incl. future** | **216,704** | |

**Recommended bucket 1 = 262,144 B**, relabelled *"KOS runtime heap, NOT image
bytes"*. **Frees ~738 KB.**

No romdisk is linked (no `__kos_romdisk` in the map) — [M], and it must stay
that way (`kb/research-size-reduction.md` §3.6).

### 3.3 GLdc costs zero today [M]

`dc/Makefile:441` passes `-lGL` and the map shows
`LOAD /opt/toolchains/dc/kos-ports/lib/libGL.a` at line 45413 — but **zero
`libGL.a(…)` members are pulled in.** Nothing references a GL symbol:
`dc/src/dc_gx.c` contains **0** `gl*` calls and targets raw PVR (still stubbed
at `dc_gx.c:1913-1940`). The `-lGL` on the link line is dead weight.

The concern that GLdc holds large main-RAM vertex buffers is **not borne out**
for its init path: a linked GLdc test binary's statics total ≈5.4 KB
(`_OP_LIST`/`_PT_LIST`/`_TR_LIST` 96 B each, `_ATTRIB_LIST` 132 B,
`_SHARED_PALETTES` 256 B, `_pool_header` 4,116 B), with submission buffers
being `AlignedVector` headers grown on demand. Linking GLdc adds **+25,388 B**
of image (a floor — LTO dead-strips unreferenced entry points).

**[?] GLdc's vertex-buffer growth policy and peak are UNVERIFIED** — the SDK
image ships only LTO bytecode and headers, no GLdc sources. Re-open this at M2
when stage A lands.

### 3.4 Buckets 9, 10, 11 are `.bss`, not heap [M]

`dc/src/dc_mem_ledger.c:64-75` marks `AUDIO`, `DISC_IO` and `PVR_STAGING` with
`carve = 1` (they would `malloc(budget)` a real extent), but every call site
only calls `dc_mem_note()` on a **static** buffer:

| bucket | budget | what actually exists | where |
|---|---:|---|---|
| 9 AUDIO | 700,000 | `jaudio_NES` `.bss` = **1,265,101** (`audiomemory` 589,824 · `seq` 277,580 · …) + `dc_audio.c` counters | static `.bss`, in image; `dc_audio.c:105` only notes |
| 10 DISC_IO | 384,000 | `dc_dvd.c.o` `.bss` = **13,320** (`dvd_path_off` + `dvd_path_pool`) | static `.bss`; `dc_dvd.c:218` only notes. **No read-ahead ring exists** |
| 11 PVR_STAGING | 384,000 | `dc_gx.c.o` `.bss` = **334,764** (`vertex_buffer[8192]`) | static `.bss`; `dc_gx.c:372` only notes |

So 1,468,000 B is reserved on the heap side of the budget for things that are
already counted in the image. Worse, if any of those buckets ever takes the
`dc_mem_alloc` path it will carve the extent *in addition to* the `.bss`.

**Note the trap this exposes:** `kb/research-size-reduction.md` §4.2 is right
that moving a static array to the heap saves nothing on a no-MMU sbrk machine.
Buckets 9/10/11 are that mistake encoded in the ledger. Either delete them and
fold the bytes into bucket 5, or make them real extents *and* delete the
`.bss` — never both.

Bucket 9 is also **1.81× over budget** on its real content, which no one had
noticed because the bucket was never compared to the `.bss` it describes.

### 3.5 Mounted archives cost ~3 KB of main RAM, not hundreds [M]

Both `forest_1st.arc` and `forest_2nd.arc` mount as **`JKRAramArchive`**
(`jsyswrap.cpp:532`, `:561`), so their payload goes to ARAM, not the arena.
But `JKRAramArchive.cpp:167` allocates `mArcInfoBlock` — the RARC node, file
entry and string tables — from **main RAM**, resident for the whole game.

Measured by reading the RARC headers straight out of the user's ISO:

| archive | disc size | nodes | file entries | **resident main-RAM table** |
|---|---:|---:|---:|---:|
| `forest_1st.arc` | 852,896 | 2 | 29 | **1,088** |
| `forest_2nd.arc` | 4,132,608 | 2 | 57 | **1,888** |
| `famicom.arc` | 1,699,904 | 4 | 51 | 1,952 |

**2,976 B total** for the two mounted archives. This was a plausible hidden
cost and it is negligible — closed. (The archives hold only 86 files between
them, so RARC granularity is very coarse; that matters for bucket 8's window
design, not for RAM.)

### 3.6 `.rodata` — the `s_assets[]` string pool figure is exact [M]

Per-tree attribution from the map (project objects only, current ELF):

| tree | `.data` | `.rodata` | total |
|---|---:|---:|---:|
| `src/data/model` | 1,123,275 | 47,780 | 1,171,055 |
| **`pc/src`** | 48 | **893,136** | **893,184** |
| `src/data/npc` | 567,762 | 1,944 | 569,706 |
| `src/data/field` | 524,056 | 24,509 | 548,565 |
| `src/actor` | 135,231 | 7,535 | 142,766 |
| `src/game` | 99,822 | 19,687 | 119,509 |
| … | | | |
| **TOTAL** | **2,629,081** | **1,036,765** | **3,665,846** |

Largest single objects: `pc/src/pc_assets.c` **888,853** ·
`src/data/field/bg/acre/bg_data.c` 317,424 · `src/data/model/player_anim.c`
250,433.

**The 888,853 B `s_assets[]` name-string claim is confirmed to the byte.**
`.rodata` outside `pc_assets.c` is only 147,912 B, so
`kb/research-size-reduction.md`'s post-fix target of 165,000 B for `.rodata`
is **achievable and correctly sized** — that lever is real.

### 3.7 `.bss` composition, current ELF [M]

`.bss` = 12,415,508. `src/data/**` = **8,519,191 (62.6 %)** across **2,534
objects**, median 2,784 B, mean 3,361 B; only 15 objects are ≥32 KB and they
total 864,096 B. Non-`src/data` `.bss` = 3,896,317 B.

The flat, fine-grained distribution is the important part: there is no big
single win in `src/data`, which is exactly why demand residency (a mechanism)
is the only lever that reaches it.

### 3.8 Accounting note that has already caused confusion

`kb/mem-budget.md` §8.1's "`.text` 6,318,568" is **not** the `.text` section.
Measured today: `.text` alone = 5,257,344; `.rodata` = 1,053,740;
`.init/.fini/.ctors/.dtors/.eh_frame/.gcc_except_table/.got` = 7,556.
`5,257,344 + 1,053,740 + 7,556 = 6,318,640` ≈ the quoted figure. **Treat
"6,318,568" as text+rodata+misc-read-only and do not add `.rodata` to it
again.**

### 3.9 Premises confirmed unchanged

- `-DTARGET_PC` **is** defined for the Dreamcast build — `dc/Makefile:164`,
  alongside `-DTARGET_DC`. The Makefile comment (`:158-162`) states the intent:
  "*Here it means 'not GameCube', not 'PC'*", guarding the base port's
  little-endian correctness fixes. It is deliberate, and documented as
  non-negotiable. **The audit of what else it drags in is unfinished — §6.2.**
- The §8.3 `.bss` fixes are present in the tree: `src/game/m_play.c:54-64`
  (`prbuf` `u16` under `TARGET_DC`) and
  `include/libforest/emu64/texture_cache.h:76-86`.
- `dc/build/bsssym.tsv` is from the **first** link (it shows `prbuf` at
  1,228,800). The ELF and map next to it are **post-fix**. Do not mix them.

---

## 4. Revised ledger (proposed replacement for `dc/include/dc_mem_budget.h`)

Do not apply blind — bucket 6 is still `[?]`.

| # | bucket | current | proposed | tag | note |
|---:|---|---:|---:|---|---|
| 1 | KOS | 1,000,000 | **262,144** | [M] | runtime heap only; image bytes are in buckets 3–5 |
| 2 | low RAM | 65,536 | 65,536 | [S] | not spendable |
| 3 | image `.text`+`.rodata` | 2,600,000 | **6,318,568** | [M] | **unachievable as budgeted.** Fixed at `-O0`, minus the 888,853 B string pool → ~5,430,000 target |
| 4 | image `.data` | 1,800,000 | 700,000 | [?] | needs §6.4 |
| 5 | image `.bss` | 1,950,000 | 1,950,000 | [?] | absorbs buckets 9–11 if they stay static |
| 6 | JKRHeap/`__osMalloc` arena | 4,000,000 | **[?] 2,750,000–5,500,000** | **[?]** | §2. −1,294,497 available free |
| 7 | asset pool | 1,500,000 | 1,500,000 | [?] | future; must be paired with the `.bss` credit |
| 8 | ARAM graph window | 512,000 | 512,000 | [?] | unmeasured |
| 9 | audio | 700,000 | **delete** | [M] | it is `.bss` — fold into 5 |
| 10 | disc I/O | 384,000 | **delete** | [M] | it is `.bss` — fold into 5 |
| 11 | PVR staging | 384,000 | **delete** | [M] | it is `.bss` — fold into 5 |
| 12 | stacks | 131,072 | 65,536 | [S] | KOS's 64 KB is already in bucket 2's line |

Also fix the header's own contract: `dc_mem_ledger.c` should assert
`image_span + additive_heap ≤ 16,646,144`, not
`sum(all buckets) ≤ 16,777,216` — the latter cannot detect the double-count
this audit found.

---

## 5. Cheapest actions, ranked

1. **Kill the XFB allocation** — `TARGET_DC` branch in `JUTXfb::initiate`
   returning a dummy non-NULL pointer. **−1,228,800 B of bucket 6.** No
   measurement needed; the buffers are provably never read (§2.2).
2. **Kill / reclaim the GX FIFO** — **−65,697 B.** §2.2(b).
3. **Fix bucket 1** — **−737,856 B of phantom reservation.** §3.2.
4. **Delete buckets 9/10/11** — **−1,468,000 B of phantom reservation.** §3.4.
5. **Drop `-lGL`** from `dc/Makefile:441` until M2 — cosmetic (0 bytes), but
   it stops the next reader budgeting for GLdc twice.
6. **Then measure bucket 6** (§2.4) before anything else is planned on it.

Items 1–4 are ~3.5 MB of the ~3.4 MB improvement in §1.3 and none of them
requires touching `src/` codegen.

---

## 6. Not finished — numbered, with next steps

**Three parallel investigations were still running when the session ended.
Their results are lost; the questions below are open.**

1. **Bucket 6's real high-water mark. THE priority.** §2 characterises the
   arena and finds 1.29 MB of dead weight, but the `__osMalloc` peak is
   unknown. **Next step: §2.4, on the Anbernic build.** Also still unanswered:
   are `gamealloc.c` / `TwoHeadArena.c` / `THA_GA.c` live in this build or
   dead libultra-era code, and do they carve from the same arena? Not checked.
   *Note:* whether an Anbernic host build tree is already configured (and how
   long a build takes) was not established either — check
   `pc/CMakeLists.txt`, `build_make.sh`, `pc/build/`.

2. **The `-DTARGET_PC` audit (question 5).** Only the premise is confirmed
   (§3.9). **The valuable part — a complete list of what the define changes —
   was not produced.** Next steps, in order:
   a. Check whether `src/data/**/assets/*.inc` files **exist in the repo**. If
      they do not, the non-`TARGET_PC` branch does not build and "revert to
      the GameCube path" is not an available option at all. This one `ls`
      decides the whole question.
   b. **Do not expect a saving from turning it off.** Under the
      non-`TARGET_PC` branch those arrays become *initialised* data — the same
      resident bytes, moved from `.bss` to `.data`, plus disc bytes. On a
      no-MMU sbrk machine that is neutral at best. The "8.5 MB is free PC
      scaffolding" reframing is therefore **probably FALSE as a RAM lever**,
      though it may still be true as a description of history. Verify before
      repeating either version.
   c. Bucket the ~2,633 `TARGET_PC` files into the mechanical `src/data`
      placeholder pattern vs the ~60–100 genuine behavioural forks in
      `src/game`, `src/static`, `src/system`, `src/effect`, `src/furniture`,
      `src/actor`, and classify the latter (endianness, pointer size, asset
      loading, file I/O, stubbed GC hardware, debug). Flag any that assume
      SDL, a host filesystem, or 64-bit.

3. **`.data`/`.rodata` const-ness and dedup (question 6).** Per-tree sizes are
   in §3.6; the analysis is not done. Next steps:
   a. Both `.data` and `.rodata` are resident RAM on DC, so adding `const`
      **saves zero by itself** — the deliverable is *bytes that are
      read-only-in-practice AND pointer-free*, i.e. evictable to disc.
   b. The 948,688 B pointer-free figure came from **dynamic relocations on the
      ARM PIE build**. That method does not exist on a static non-PIE SH ELF,
      so it is **unverified for this target**. Re-derive by scanning `.data`
      contents for words in `0x8c010000.._end` and classifying per symbol;
      report false-positive risk honestly.
   c. Exact-duplicate dedup of generated `src/data` tables (hash the
      initialised bytes per symbol, group). `--icf` is unavailable on SH but
      source-level dedup in the generator is allowed. **Completely unmeasured
      — could be zero, could be significant.**

4. **Bucket 8 (ARAM graph window, 512,000).** Still a guess. §3.5 shows the
   archives hold only 86 coarse files, which may make a small window unworkable
   — a single `forest_2nd.arc` member could be large. **Next step: list the
   member sizes from the RARC file-entry table (the reader in §3.5 already
   parses the header) before sizing the window.**

5. **Bucket 5's post-conversion floor.** §3.7 gives 3,896,317 B of
   non-`src/data` `.bss` today against a 1,950,000 B budget. The itemised path
   from one to the other (`kb/mem-budget.md` §4.1) predates the DC link and was
   not re-checked.

6. **Nothing here was executed.** Every runtime figure in §3.1/§3.2 is
   static source reading, and §2's arena consumption is static analysis of a
   binary that **has never booted**. A boot-time `mallinfo()` /
   `arch_get_ram_free()` probe after `pvr_init` would convert most of §3 from
   [S] to [M] cheaply, and should be the first thing M1 does once the image
   fits.

---

## 7. Bottom line

- The required cut is **~11.07 MB**, not 14.45 MB — and **~1.1 MB of that gap
  was just staleness**, not analysis. Roughly 2.3 MB more is real: the budget
  was double-counting.
- **A further 1.29 MB of bucket 6 is provably dead weight** (dead framebuffers
  and a dead GX FIFO) and needs no measurement to remove.
- **But bucket 6's actual requirement is still unknown**, and the only
  historical evidence found — a dead `0x380000` default — points at ~3.5 MB,
  i.e. *against* the hoped-for 1.5 MB. **Do not budget on the optimistic
  reading.**
- The binding constraint is unchanged and unforgiving: with `-O0` frozen,
  `.text` + `.data` = 8,957,420 B leaves **1,349,044 B for all of `.bss`**,
  which is 12,415,508 B today. **`src/data` demand residency is still ~76 % of
  the cut and there is no second lever within a factor of eight of it.**
