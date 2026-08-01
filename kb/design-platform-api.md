# Platform API surface — what `dc/` must provide

Recon pass, 2026-08-01. Derived by reading `pc/CMakeLists.txt`, every
`pc/src/*.c` and `pc/include/*.h`, and cross-checking against call sites in
the vendored `src/` and `include/`. **Nothing here is guessed** — every symbol
below is a non-static definition that exists today in `pc/src/`; every
"N call sites" figure is a grep over the compiled (non-excluded) subset of
`src/`. Where something could not be verified it says so.

Companion docs: `PLAN.md` (§3 the four hard problems), `kb/base-repo-map.md`
(what the base repo contains), `kb/research-dreamcast.md` (KOS/PVR facts).

---

## 0. Summary

| | count |
|---|---|
| Non-static symbols defined by `pc/src/` | **698** |
| …referenced by name somewhere in `src/` or `include/` | 589 |
| …`pc/`-internal only (not named by game code) | 109 (marked ✱ in the tables) |
| Distinct subsystem groups | 55 |

Dispositions:

| disposition | count | meaning |
|---|---|---|
| `rewrite-for-KOS` | **421** | needs a genuinely new DC implementation |
| `port-as-is` | **146** | the body compiles and behaves correctly on sh-elf unchanged |
| `drop` | **97** | no caller in the DC build; delete rather than port |
| `stub-and-log` | **34** | keep an empty body, add a log so we notice if it is ever hit |

The 421 "rewrite" figure is dominated by the GX surface (≈300 symbols) —
most of those are one-line state setters whose rewrite is mechanical. The
genuinely hard rewrites are enumerated in §4.

### What the build actually compiles (from `pc/CMakeLists.txt`)

`GLOB_RECURSE src/**.c` + `src/**.cpp`, then these are **excluded** and must
therefore be reimplemented by the platform layer:

- Whole Dolphin SDK subtrees: `dolphin/{gx,os,vi,pad,card,dvd,ai,ar,dsp,exi,si,db,mtx,base,gba,amcstubs,OdemuExi2,odenotstub}`
  — everything in the tables below with a `GX*`/`OS*`/`VI*`/`PAD*`/`CARD*`/`DVD*`/`AI*`/`DSP*`/`AR*`/`EXI*`/`SI*`/`DB*`/`PSMTX*`/`C_MTX*` name.
- **Exception:** `src/static/dolphin/pad/Padclamp.c` is explicitly added back
  into `PC_SOURCES`. It is the ONE Dolphin SDK source file the port keeps
  (`JUTGamePad.cpp` calls `PADClamp`). `dc/` must do the same.
- PPC/CodeWarrior runtime: `TRK_MINNOW_DOLPHIN/`, `Runtime.PPCEABI.H/`,
  `MSL_C.PPCEABI.bare.H/`, `MSL_C/`, `ReconfigBATs.c`, `executor.c`.
- `libultra/` (all) — replaced by `os*` shims in `pc_os.c`/`pc_stubs.c`;
  `libc64/{malloc,sprintf,aprintf}.c`; `osreport.c`.
- `Famicom/` (PPC asm NES core) — replaced by `famicom_*` stubs.
- `src/game/m_card.c` — **replaced wholesale by `pc/src/pc_m_card.c`**. This
  is the only *game* source file the platform layer reimplements, and it is
  1442 LOC of save/village/travel logic, not a hardware shim.
- `AUS/`, `src/furniture/*` (folded into `f_furniture.c`), various
  `#include`-only `.c` fragments, `ef_effect_lib.c`,
  `jaudio_NES/internal/dsp_{cardunlock,GBAKey}.c`, `emu64_print.cpp`,
  `emu64_utility.c`, `jsyswrapper_{ext,main}.cpp`, `src/data/model/obj_e_boat/`.
- `src/main.c`'s `main()` → `ac_entry` and `src/static/boot.c`'s `main()` →
  `boot_main` via `-Dmain=…`. The platform layer owns the real `main()`.

### The platform surface is NOT only `pc/`

**84** non-data, non-furniture `.c`/`.cpp` files under `src/` carry
`#ifdef TARGET_PC` / `#ifndef TARGET_PC` conditional code (2681 files
including headers). The biggest and most load-bearing is
`src/static/jaudio_NES/internal/audiothread.c`, where lines 19–110 are a
complete TARGET_PC replacement for the GC threading model — it is why
`OSCreateThread`, `OSSetCurrentContext`, `__OSSetInterruptHandler`,
`OSSleepThread`, `OSExitThread` and friends are *called* in the source but
never *needed* at link time. `dc/` inherits this: the DC port either keeps
`TARGET_PC` defined (and lives with a single-threaded audio model) or
introduces `TARGET_DC` and re-opens all 84 files. **Recommendation: keep
`TARGET_PC` defined for M1 so the link closes, and add `TARGET_DC` only
where DC needs to differ.**

---

## 1. Ordering constraints (from `pc_main.c` → `boot.c` → `jsyswrap.cpp`)

Verified boot order. Anything that reads a value must come after whatever
writes it; the ✱ items are the hard constraints.

```
main()                                       [pc_main.c]
 1. setvbuf(stdout), parse argv
 2. ✱ compute image base/end (pc_image_base/_end)   ← seg2k0 pointer heuristic
 3. pc_settings_load()                              ← must precede window creation
 4. pc_keybindings_load()
 5. pc_platform_init()
      SDL_Init → window → GL context
      pc_overlay_init(); boot splash
      ✱ pc_gx_init()        ← must precede any GX call
      pc_texture_pack_init()
 6. pc_check_disc_or_die()  → pc_disc_init()   ✱ must precede pc_assets_init
 7. ✱ pc_assets_init()      ← fills every .inc asset the linked game references,
                              BEFORE any game code runs
 8. ac_entry()              (src/main.c main(); sets HotStartEntry = &entry)
 9. boot_main(argc, argv)   (src/static/boot.c main())
      ReconfigBATs()
      InitialStartTime = osGetTime()      ✱ time source live before OSInit()
      OSInit()                            ✱ allocates the arena, writes *(arena+0x28)
      OSInitAlarm()
      ✱ OSGetStackPointer() / OSGetCurrentThread()->stackBase/stackEnd
        are DEREFERENCED here to paint the stack with 0xFD
      bzero(osAppNMIBuffer, 64B); OSGetResetCode()
      __osInitialize_common(); OSGetConsoleType()
      DVDGetCurrentDiskID()               ✱ gameVersion drives zurumode flags
      adjustOSArena()   → OSGetArenaLo/Hi, OSSetArenaHi, bzero(whole arena)
      JW_Init()                           [jsyswrap.cpp]
        SystemHeapSize = arenaHi - arenaLo - 0xD0
        JFWSystem::setAramAudioBufSize(0x810000)     ← 8.44 MB
        JFWSystem::setAramGraphBufSize(0x6A3780)     ← 6.96 MB
        JFWSystem::setFifoBufSize(0x10001)
        JFWSystem::init()                 [JFWSystem.cpp]
          firstInit(): OSInit(); DVDInit();
                       ✱ JKRExpHeap::createRoot → JKRHeap::initArena
                         → OSInitAlloc, OSPhysicalToCached(0),
                           mMemorySize = *(u32*)(start + 0x28)      ← §3.1
          ✱ JKRAram::create → ARInit(), ARQInit(), ARGetSize(), ARAlloc ×2/3
          JKRThread(OSGetCurrentThread(), 4)
          ✱ JUTVideo::createManager → VIInit, VISetBlack, VIFlush,
             VIGetRetraceCount, OSGetTick, VISetPre/PostRetraceCallback,
             OSInitMessageQueue, GXSetDrawDoneCallback
          ✱ JUTCreateFifo(0x10001) → GXInit(base, size)   ← first GX call
          ✱ JUTGamePad::init() → PADInit()
          JUTDirectPrint / JUTException / fonts / consoles
        JFWDisplay::createManager(&GXNtsc480IntDf, …)
      fault_Init() + 6 fault clients (incl. DisplayArena)
      sound_initial()   → Na_InitAudio(…) → AIInit / AIInitDMA / DSPInit
                        → msleep(2500)                ✱ 2.5 s dead wait
      initial_menu_init(); dvderr_init()
      sound_initial2()  → loop { VIWaitForRetrace(); Na_GameFrame(); }
                          until Na_CheckNeosBoot()    ✱ first frame loop
      JKRDvdToMainRam("/COPYDATE"); LoadStringTable("/static.str")
      … JW_Init2() (forest_1st.arc → ARAM), MallocInit(gameheap),
        JW_Init3() (forest_2nd.arc → ARAM) … → HotStartEntry → game loop
10. pc_disc_shutdown(); pc_platform_shutdown()
```

Hard ordering rules for `dc/`:

1. **Asset table before game code.** `pc_assets_init()` must complete before
   `ac_entry()`; the linked-in `.inc` arrays are empty until it runs.
2. **Arena before JKRHeap.** `OSInit()` must have written the memory-size word
   at `OSPhysicalToCached(0) + 0x28` before `JKRExpHeap::createRoot` runs, or
   every heap is created with a garbage `mMemorySize`.
3. **ARAM before archives.** `ARInit`/`ARAlloc` run inside `JFWSystem::init()`,
   long before `JW_Init2()` mounts `forest_1st.arc` into ARAM.
4. **`GXInit` before any other GX call**, and it happens *inside*
   `JFWSystem::init()` — i.e. after the heap exists, so the PVR/GLdc
   initialisation cannot be done lazily on first draw without care.
5. **VI callbacks are registered before the first frame** and
   `VIWaitForRetrace()` is first called from `sound_initial2()`, i.e. before
   the game loop — the frame pacing path must be live at that point.
6. **Time source before `OSInit()`** — `InitialStartTime = osGetTime()` is the
   very first line of `boot_main`, one statement ahead of `OSInit()`.

---

## 2. Which `pc/` files can be reused, and how much work each is

"Verbatim" below means *the `.c` body needs no edits*. Six of these still
`#include "pc_platform.h"`, which today pulls in `SDL.h` and `glad`; `dc/`
must supply an equivalent header (types, globals, small prototypes) before
they compile. That header is the single highest-leverage first file to write.

### 2a. Compile VERBATIM (body unchanged)

| file | LOC | why it survives | caveat |
|---|---|---|---|
| `pc_save_bswap.c` | 1029 | GCI byte-swap tables; SH-4 is LE exactly like the base targets | includes only decomp headers + `<string.h>` — **truly verbatim** |
| `pc_stubs.c` | 115 | pure empty stubs over `types.h` | **truly verbatim** (no `pc_platform.h`) |
| `pc_stubs_cpp.cpp` | 34 | JSystem C++ vtable bodies the decomp declares but never defines | needs `pc_platform.h` replacement only for types |
| `pc_mtx.c` | 571 | `PSMTX*`/`C_MTX*`/`gu*` in scalar C | correct as-is; §4.5 wants FTRV/FIPR rewrites of ~6 hot functions **later** |
| `pc_misc.c` | 222 | register-array dummies, PPC/EXI/SI stubs, `sins`/`coss`, malloc-arena bookkeeping | one `fprintf`; `bzero`/`bcopy` are `#ifdef _WIN32` (newlib provides them — *unverified for KOS*) |
| `pc_aram.c` | 81 | compiles unchanged | **but semantically wrong on DC** — 16 MB `malloc` cannot exist; see §4.3 |
| `pc_assets.c` | 30677 | generated dispatch table, plain C | its data source changes (ISO → `/cd`), the table does not; regenerate with `pc/tools/gen_runtime_assets.py` |

### 2b. Small edits (swap one backend, keep the logic)

| file | LOC | edit |
|---|---|---|
| `pc_prof.c` | 40 | `clock_gettime(CLOCK_MONOTONIC)` → KOS timer; `getenv` → compile-time flag (*KOS `clock_gettime` support unverified*) |
| `pc_dvd.c` | 247 | `fopen/fseek/fread` → KOS VFS `/cd`; add the read-ahead thread. Entry-table interning and the `DVDFileInfo` +0x18/+0x30/+0x34 layout stay |
| `pc_card.c` | 393 | file-per-GCI backend → VMU (`vmufs`); the 29 `CARD*` entry points and the 20-byte `CARDFileInfo` layout stay |
| `pc_m_card.c` | 1442 | pure game logic above `CARD*`; only the save-size/compression policy changes |
| `pc_settings.c` | 472 | drop SDL window calls; keep the ini parser or replace with compile-time defaults. `pc_settings_cull_limit_xz` is called from game code |
| `pc_vi.c` | 248 | frame-pacing/dynamic-FPS logic is portable; swap + retrace become `pvr_scene_finish()`/`vid_waitvbl` |
| `pc_audio.c` | 214 | ring buffer + `AI*`/`DSP*` surface stays; SDL device → KOS `snd_stream` |
| `pc_disc.c` | 445 | **moves to `tools/`** as host code — same source, different build |

### 2c. Rewrite from scratch

| file | LOC | why |
|---|---|---|
| `pc_gx.c` | 2685 | GL half dies. Portable half (batch merge, state dedup, strip/fan→list conversion, whole-batch frustum cull) transplants into the new backend |
| `pc_gx_texture.c` | 1227 | decoders survive; output stage becomes twiddled 16-bit / paletted / VQ in VRAM instead of linear RGBA8 |
| `pc_gx_tev.c` | 897 | **deleted** — TEV→GLSL generator, shader cache, shader seed. No shaders on PVR |
| `pc_main.c` | 619 | SDL/GL init, signal-based crash recovery → KOS init + SH-4 exception handler |
| `pc_pad.c` | 308 | SDL GameController → maple |
| `pc_os.c` | 495 | mmap arena → static arena; cache ops become real; address translation becomes real |
| `pc_overlay.c` | 1247 | GL overlay renderer — **drop** for now |
| `pc_texture_pack.c` | 1239 | HD texture packs — **drop** (non-goal) |
| `pc_model_viewer.c` | 891 | dev tool — **drop** |
| `pc_typing.c` / `pc_keybindings.c` | 148 / 255 | SDL text input → DC keyboard via maple; **drop** for M1 |
| `glibc_compat.c` | 34 | glibc symbol-version shims — **delete** (newlib) |

---

## 3. Semantics that will bite (read before writing any `dc/` code)

### 3.1 The arena and the `+ 0x28` word

`pc_os.c:283` writes `*(u32*)(arena_memory + 0x28) = PC_MAIN_MEMORY_SIZE;`.

Why: on real hardware the GameCube boot info block lives at physical address
0, and offset `0x28` holds the console's memory size. `JKRHeap::initArena`
(`src/static/JSystem/JKernel/JKRHeap.cpp:64-69`) does exactly this:

```c
u8* start = (u8*)OSPhysicalToCached(0);
mCodeStart   = start;
mCodeEnd     = (u8*)arenaLo;
mUserRamStart= (u8*)arenaLo;
mUserRamEnd  = (u8*)arenaHi;
mMemorySize  = *(u32*)((start + 0x28));      // ← the word
```

The PC port makes `OSPhysicalToCached(p)` return `arena_memory + p`, so
"physical 0" *is* the base of the mmap'd arena, and the port has to
manufacture the 0x28 word there itself. Every `JKRHeap` in the game inherits
`mMemorySize` from it; `JKRHeap::getTotalFreeSize`/heap reporting and the
`JUTProcBar` heap bar read it.

**On Dreamcast:** KOS main RAM starts at `0x8C000000` and the first ~64 KB
belong to KOS/the IP.BIN loader. Two options, decide once and write it down:

- **(A) Keep the PC trick.** `OSPhysicalToCached(p) == dc_arena_base + p`,
  and reserve the first 0x2C bytes of the arena for the fake boot block. Zero
  KOS interaction, identical to the proven base port. Costs 44 bytes.
- **(B) Real mapping.** `OSPhysicalToCached(p) == 0x8C000000 + p`, then
  `start+0x28` is inside KOS's low memory and must not be written.

**(A) is strongly recommended** — it also keeps `OSCachedToPhysical` a simple
subtraction, which emu64's pointer plumbing depends on.

Related: `adjustOSArena()` in `boot.c` `bzero`s the *entire* `arenaLo..arenaHi`
span at boot. On a 9 MB DC arena that is a ~9 MB memset every cold boot —
measurable but acceptable; on a 24 MB arena it is not.

Also related: `JW_Init` computes `SystemHeapSize = arenaHi - arenaLo - 0xD0`,
so the DC arena size *is* the system heap size. There is no separate knob.

### 3.2 Cache maintenance — no-ops on x86/ARM, REAL on SH-4 ★

Every one of these is `{ (void)addr; (void)len; }` in `pc_os.c` today. On
SH-4 with PVR store-queue DMA and AICA DMA they are load-bearing. Call-site
counts are over compiled (non-excluded) `src/`:

| symbol | call sites | where | SH-4 mapping |
|---|---|---|---|
| `DCStoreRangeNoSync` | **25** | jaudio (`dspbuf`, `cpubuf`, `aramcall`), JKRAram | `dcache_flush_range` (writeback) |
| `DCFlushRange` | **12** | `m_card.c`, `dspinterface.c`, `fxinterface.c`, `JUTDirectPrint` | `dcache_flush_range` (writeback + inval) |
| `DCStoreRange` | **10** | **`emu64.c:965` (decoded texture) and `emu64.c:1004` (TLUT)**, `aictrl.c` DAC buffers, `cpubuf`, `dspbuf` | writeback |
| `DCFlushRangeNoSync` | **10** | jaudio | writeback |
| `DCInvalidateRange` | **9** | `JKRAramPiece.cpp:97` (DMA completion), `dspinterface.c`, `JFWDisplay.cpp` XFB | `dcache_inval_range` |
| `osWritebackDCache` | **8** | libultra-flavoured game code | writeback |
| `DCTouchRange` | 6 | prefetch hint | safe as a no-op |
| `DCZeroRange` | 1 | `memset` is a correct (slower) substitute | — |
| `ICInvalidateRange`, `ICFlashInvalidate`, `LCEnable`, `LCDisable` | **0** | — | drop |

Two SH-4 traps:

- **Invalidate discards dirty lines.** `dcache_inval_range` on a range that
  is not 32-byte aligned will drop neighbouring dirty data. GC's
  `DCInvalidateRange` had the same constraint and the game's call sites are
  believed aligned, but this is exactly the class of bug that shows up as
  "audio buffer has one wrong sample every N frames". Assert alignment in
  the DC implementation during bring-up.
- **`emu64.c:965/1004` are the texture path.** Those two `DCStoreRange`
  calls are the game telling the platform "the texture bytes are ready".
  They are the correct hook for "upload to VRAM"; treating them as no-ops
  and instead uploading on `GXLoadTexObj` also works, but pick one.

`OSCachedToUncached`/`OSUncachedToCached` are identity functions on PC. On
SH-4 they are the P1↔P2 window flip (`^ 0x20000000`) and are the *other*
correct way to keep DMA-visible buffers coherent.

### 3.3 `OSGetTime` / `OSGetTick` conventions

- GC tick rate = bus clock / 4 = 162 MHz / 4 = **40.5 MHz**
  (`GC_TIMER_CLOCK` in `pc_platform.h`). Every timeout, animation delta and
  RTC computation in the game is denominated in these ticks. **Do not change
  the rate** — change the source only.
- `OSGetTime()` returns absolute ticks since the **GameCube epoch,
  2000-01-01 00:00:00 local time** (`GC_UNIX_EPOCH_DIFF = 946684800`), with
  the host timezone offset folded in at `OSInit()` time. Animal Crossing's
  entire calendar/event system reads this.
- `OSGetTick()` / `osGetCount()` return the **low 32 bits**. The game uses
  `OSDiffTick(a,b) = (s32)a - (s32)b`, which wraps every ~106 s at 40.5 MHz.
  Anything that measures longer than that is already broken on GC too.
- `pc_os.c` splits whole seconds from the remainder before scaling
  (`(diff/freq)*CLK + (diff%freq)*CLK/freq`) because the naive form
  overflows u64 after ~455 s at a 1 GHz counter. Keep the split.
- **DC source:** the Dreamcast has a battery-backed RTC. KOS exposes
  `rtc_unix_secs()`; the DC RTC epoch is 1950-01-01, so KOS's conversion plus
  the GC-epoch offset gives wall-clock time. A monotonic high-resolution
  counter (KOS `timer_ns_gettime64` / TMU) provides the tick delta. *The
  exact KOS API names have not been verified in this pass — confirm against
  the KOS tree during M0.*
- `boot.c` calls `osGetTime()` **before** `OSInit()`. Whatever provides the
  monotonic base must be safe to call before init.

### 3.4 The ARAM DMA contract ★

`ARStartDMA(type, mram_addr, aram_addr, length)` is a `memcpy` today, and it
is the single seam between the game and the 16 MB of auxiliary RAM the
Dreamcast does not have. Three behaviours are load-bearing:

1. **ARAM addresses are offsets, not pointers.** `ARInit` returns 0;
   `ARGetBaseAddress()` returns 0; `ARAlloc` is a 32-byte-aligned bump
   allocator that never frees.
2. **Defensive pointer normalisation.** Some callers pass
   `aram_base + offset` instead of a bare offset. `pc_aram.c:45-48` detects
   an `aram_addr` inside `[aram_base, aram_base+16MB)` and subtracts the
   base. Removing this will break those callers.
3. **Out-of-range reads zero-fill.** For `type == 1` (ARAM→MRAM) with an
   out-of-range source, the destination is `memset` to 0 (capped at 1 MB)
   rather than left as garbage. Game code depends on getting zeros.

`ARQPostRequest` has an **argument-order trap**: its `(source, dest)` are not
`ARStartDMA`'s `(mram, aram)`. For `type == 0` it forwards
`(source→mram, dest→aram)`; for `type == 1` it **swaps** them. The completion
callback is invoked synchronously with the request pointer cast to `u32`.

Consumers: `JKRAram`/`JKRAramArchive`/`JKRAramPiece` (graph half, 6.96 MB of
RARC archives) and jaudio (`aramcall.c`, sound half, 8.44 MB). PLAN §3.1 turns
the graph half into a disc-backed LRU window — **`ARStartDMA` is where that
window is implemented**, and it is the only place it needs implementing.

### 3.5 DVD: async faked as sync ★

`DVDReadAsyncPrio(fileInfo, buf, len, off, callback, prio)` performs the whole
read **inline** and calls `callback(nread, fileInfo)` **before returning
TRUE**. Every "async" DVD path in the game therefore completes before the
caller's next statement. `DVDGetCommandBlockStatus` always returns 0
(`DVD_STATE_END`) for the same reason.

This has been validated across a full playthrough on the base port, so it is
safe — but it means:

- Any real read-ahead thread on DC must **preserve callback-before-return**,
  or every caller that polls a "done" flag needs re-auditing.
- The read-ahead has to be a *prefetch* layer under a still-synchronous
  `DVDRead`, not a genuine async API. That is also the right shape for
  CD-R at ~500 KB/s.

Two more layout facts:

- `DVDConvertPathToEntrynum` **interns** unknown paths into a growing table
  (`MAX_DVD_ENTRIES = 512`) and returns the index — it never returns -1 for
  an unknown file the way real GC does. Failure is deferred to `DVDFastOpen`.
- `DVDFastOpen` writes into the game's 0x3C-byte `DVDFileInfo` at hand-picked
  offsets: handle/sentinel at **+0x18**, `startAddr` at **+0x30**, `length` at
  **+0x34**. Those offsets come from `dolphin/dvd.h` and must not move.

### 3.6 The GX vertex/state machine

The GX layer is not a thin wrapper; it is a state machine with four
behaviours the DC backend must reproduce:

1. **`GXEnd` is never called.** emu64 omits it. Batches terminate when the
   declared vertex count from `GXBegin(prim, fmt, nverts)` is reached
   (`pc_gx_flush_if_begin_complete`). Strip/fan batches complete on their
   **source** count while the emitted count is 3×(n−2).
2. **Deferred vertex commit.** A `GXPosition*` call commits the *previous*
   vertex and starts a new one. The reset clears normal, color1 and texcoords
   but **carries color0 forward**. Break this and vertex colours break.
3. **Batch merging.** `GXBegin` concatenates into an already-open, already-
   complete batch when `dirty == 0`, the primitive is QUADS or TRIANGLES, and
   the vtxfmt matches. Measured: 491–600 draws/frame → **114.6**.
4. **State dedup + whole-batch frustum cull.** Re-setting identical state
   returns early (no flush, no dirty bit), because the decomp re-sets state
   constantly. The cull tests an object-space AABB against the exact P·MV and
   rejects **60–80 %** of submitted batches (~286/frame). Both are pure C and
   both are exactly what a tile-based deferred GPU wants — transplant them
   unchanged.

Where the batches break today (why merging is rarer than it could be):
modelview loads **41 %**, texture changes **30 %**. `GXLoadPosMtxImm` is the
single biggest batching obstacle; a CPU pre-transform pass would help DC even
more than it helps the base port.

Indexed attributes: `GXSetArray(attr, ptr, size, stride)` stores a raw
GameCube-era 32-bit pointer, and `GXPosition1x16`/`GXNormal1x16`/
`GXTexCoord1x16`/`GXColor1x16` fetch through it assuming `f32[3]` /
`f32[2]` sources. Those pointers must survive the seg2k0 range heuristic
(PLAN §11.6).

TEV: max **3** stages (`PC_GX_MAX_TEV_STAGES`), **101** unique configurations
harvested from a full playthrough. That is a complete, finite spec — it is
what `tev_map.md` (M2) must classify.

TLUT: per-slot `is_be` flag. ROM-sourced palettes are big-endian; emu64/EFB
ones are native little-endian. `pc_gx_tlut_set_native_le(idx)` is the setter.
This distinction carries to DC unchanged.

### 3.7 Threads: the port is single-threaded, and the game knows

`osCreateThread2`/`osStartThread` deliberately **do not start anything**;
`OSCreateThread` returns FALSE; `OSDisableInterrupts` returns 0.
`OSSendMessage` is non-blocking and returns FALSE when full (real Dolphin
blocks when `flags == OS_MESSAGE_BLOCK`). The one real thread on PC is the
SDL audio producer, created by `pc_audio_start_producer_thread()` and driving
`pc_audio_process_frame()` — which lives in
`src/static/jaudio_NES/internal/audiothread.c` under `#ifdef TARGET_PC`.

Two consequences for DC:

- `OSGetCurrentThread()` must return a pointer whose `stackBase`/`stackEnd`
  fields are readable — `boot.c` dereferences them at startup. PC returns a
  512-byte zeroed dummy.
- If DC introduces a real KOS audio thread, `OSDisableInterrupts` /
  `OSInitMessageQueue` / `OSSendMessage` stop being safe as stubs, and the
  `OSMessageQueue` struct layout (16 bytes of thread queues, then
  `msgArray`/`msgCount`/`firstIndex`/`usedCount`) must be honoured because
  the game allocates those structs itself.

### 3.8 Saves

`GCI_FILE_DATA_SIZE = mCD_LAND_SAVE_SIZE = 0x72000` = **466,944 bytes**
(≈456 KB), sector size `0x2000`. Layout inside the file: others block at 0,
main save at `0x26000`, backup save at `0x4C000`. VMU user space ≈ 100 KB.
`pc_save_bswap.c` keeps the on-disk image big-endian so GCI files interchange
with Dolphin — that stays true on SH-4 (also LE), and is worth preserving
regardless of what the on-VMU format ends up being (PLAN §6).

---

## 4. Top semantic landmines (ranked)

1. **`OSPhysicalToCached(0) + 0x28`** — the JKRHeap memory-size word. Get the
   physical-address mapping wrong and every heap in the game is created with
   a garbage size, at boot, silently. §3.1.
2. **Cache ops are real on SH-4.** 66 call sites currently compiled to
   nothing, several of them directly on buffers the PVR and AICA will read
   (`emu64.c:965/1004`, `aictrl.c` DAC buffers, `JKRAramPiece` DMA
   completion). Invalidate is line-granular and discards dirty lines. §3.2.
3. **`ARStartDMA` is the whole 16 MB ARAM problem in one function** — plus an
   argument-order swap in `ARQPostRequest` and two defensive behaviours
   (pointer normalisation, zero-fill on OOB) that callers rely on. §3.4.
4. **`GXEnd` is never called and `GXPosition*` carries color0 forward.** Any
   PVR vertex path that assumes a conventional begin/end or a fully-reset
   vertex will produce geometry that is subtly wrong rather than obviously
   broken. §3.6.
5. **`DVDReadAsyncPrio` fires its callback before returning.** The read-ahead
   thread that CD-R streaming requires must not turn this into a real async
   API without auditing every caller. §3.5.

Runners-up: `OSGetTick` wraps every ~106 s and the tick rate (40.5 MHz) is
baked into game timing; `OSGetCurrentThread()`'s result is dereferenced in
`boot.c`; `DVDFastOpen`/`CARDFileInfo`/`GXTexObj` are all layout-critical
structs the *game* allocates.

---

## 5. Complete symbol manifest

Legend: **✱** = the symbol name appears nowhere in `src/` or `include/`
(i.e. it is `pc/`-internal plumbing, not part of the game↔platform contract).
`pc/ file` is where the definition lives today.

### Entry point & platform lifecycle

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `pc_crash_get_data_addr` | `unsigned int pc_crash_get_data_addr(void)` | pc_main.c | rewrite-for-KOS |  |
| `pc_crash_protection_init` | `void pc_crash_protection_init(void)` | pc_main.c | rewrite-for-KOS |  |
| `pc_crash_set_jmpbuf` | `void pc_crash_set_jmpbuf(jmp_buf* buf)` | pc_main.c | rewrite-for-KOS |  |
| `pc_crash_get_jmpbuf` | `jmp_buf* pc_crash_get_jmpbuf(void)` | pc_main.c | rewrite-for-KOS |  |
| `pc_crash_get_addr` | `unsigned int pc_crash_get_addr(void)` | pc_main.c | rewrite-for-KOS |  |
| `pc_platform_init` ✱ | `void pc_platform_init(void)` | pc_main.c | rewrite-for-KOS |  |
| `pc_platform_shutdown` | `void pc_platform_shutdown(void)` | pc_main.c | rewrite-for-KOS |  |
| `pc_platform_update_window_size` ✱ | `void pc_platform_update_window_size(void)` | pc_main.c | rewrite-for-KOS |  |
| `pc_platform_swap_buffers` ✱ | `void pc_platform_swap_buffers(void)` | pc_main.c | rewrite-for-KOS |  |
| `pc_platform_poll_events` ✱ | `int pc_platform_poll_events(void)` | pc_main.c | rewrite-for-KOS |  |
| `main` | `int main(int argc, char* argv[])` | pc_main.c | rewrite-for-KOS |  |

### OS — arena / JKRHeap contract

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `OSGetArenaLo` | `void* OSGetArenaLo(void)` | pc_os.c | rewrite-for-KOS | JKRHeap::initArena reads lo/hi; must return the DC static arena. Value must be >= 32B-aligned. |
| `OSGetArenaHi` | `void* OSGetArenaHi(void)` | pc_os.c | rewrite-for-KOS | boot.c adjustOSArena() bzero()s the whole lo..hi span at boot — sizing this is the RAM ledger. |
| `OSSetArenaLo` | `void OSSetArenaLo(void* lo)` | pc_os.c | rewrite-for-KOS |  |
| `OSSetArenaHi` | `void OSSetArenaHi(void* hi)` | pc_os.c | rewrite-for-KOS |  |
| `OSInitAlloc` | `void* OSInitAlloc(void* arenaStart, void* arenaEnd, int maxHeaps)` | pc_os.c | port-as-is | Pure arithmetic (skip maxHeaps*24 descriptors, round up 32B). Compiles unchanged. |
| `OSAllocFromHeap` | `void* OSAllocFromHeap(int heap, u32 size)` | pc_os.c | rewrite-for-KOS | PC cheats with malloc(). On DC must come out of the arena or KOS malloc; __OSCurrHeap is ignored. |
| `OSFreeToHeap` | `void OSFreeToHeap(int heap, void* ptr)` | pc_os.c | rewrite-for-KOS |  |
| `OSCreateHeap` | `int OSCreateHeap(void* lo, void* hi)` | pc_os.c | rewrite-for-KOS |  |
| `OSSetCurrentHeap` | `int OSSetCurrentHeap(int heap)` | pc_os.c | rewrite-for-KOS |  |
| `OSGetPhysicalMemSize` | `u32 OSGetPhysicalMemSize(void)` | pc_os.c | rewrite-for-KOS | Returns PC_MAIN_MEMORY_SIZE (24 MB). Must return the DC arena size, and must agree with *(u32*)(OSPhysicalToCached(0)+0x28). |

### OS — time & calendar

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `osGetTime` | `s64 osGetTime(void)` | pc_os.c | rewrite-for-KOS | GC tick = bus clock/4 = 40.5 MHz. PC derives from SDL perf counter with a split-seconds trick to avoid u64 overflow (~455 s). DC: use KOS timer_ns_gettime64()/perf counter and keep the same split; the game's clock depends on ticks-per-second being exactly GC_TIMER_CLOCK. |
| `OSGetTime` | `s64 OSGetTime(void)` | pc_os.c | rewrite-for-KOS | Absolute ticks since GC epoch = 2000-01-01 (946684800 s after Unix epoch), local-time-adjusted. Dreamcast has a battery-backed RTC readable via KOS `rtc_unix_secs()` — that is the natural source; DC RTC epoch is 1950-01-01, so a second offset conversion is needed. |
| `__OSGetSystemTime` | `s64 __OSGetSystemTime(void)` | pc_os.c | rewrite-for-KOS |  |
| `osGetCount` | `u32 osGetCount(void)` | pc_os.c | rewrite-for-KOS |  |
| `OSGetTick` | `u32 OSGetTick(void)` | pc_os.c | rewrite-for-KOS | u32 truncation of the 64-bit tick. Game does OSDiffTick((s32)a-(s32)b) — wraps every ~106 s. Must keep the SAME tick rate or every timeout in the game changes. |
| `OSTicksToCalendarTime` | `void OSTicksToCalendarTime(s64 ticks, void* td_ptr)` | pc_os.c | port-as-is | Pure integer math ported from OSTime.c; BIAS=0xB2575 days. Compiles unchanged. |
| `OSCalendarTimeToTicks` | `s64 OSCalendarTimeToTicks(void* td_ptr)` | pc_os.c | port-as-is |  |

### OS — cache maintenance (REAL on SH-4)

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `DCFlushRange` | `void DCFlushRange(void* addr, u32 len)` | pc_os.c | rewrite-for-KOS | ★ REAL on SH-4. 12 call sites in compiled game code (m_card.c, jaudio dspinterface/fxinterface, JUTDirectPrint). Must become `dcache_flush_range()` (KOS) — writeback+invalidate — wherever the buffer is then touched by PVR/AICA/DMA. |
| `DCTouchRange` | `void DCTouchRange(void* addr, u32 len)` | pc_os.c | rewrite-for-KOS | 6 call sites; prefetch hint, safe to keep as a no-op. |
| `DCStoreRange` | `void DCStoreRange(void* addr, u32 len)` | pc_os.c | rewrite-for-KOS | ★ REAL on SH-4. 10 call sites (emu64.c texture/TLUT upload, aictrl DAC buffers, cpubuf/dspbuf). emu64.c:965/1004 store decoded texture+TLUT ranges — exactly the buffers the PVR texture uploader will read. |
| `DCInvalidateRange` | `void DCInvalidateRange(void* addr, u32 len)` | pc_os.c | rewrite-for-KOS | ★ REAL on SH-4. 9 call sites (JKRAramPiece DMA completion, dspinterface, JFWDisplay XFB). SH-4 has `dcache_inval_range()`; note SH-4 invalidate is line-granular and will DISCARD dirty lines — the address/length must be 32-byte aligned or data is lost. |
| `DCFlushRangeNoSync` | `void DCFlushRangeNoSync(void* addr, u32 len)` | pc_os.c | rewrite-for-KOS | ★ 10 call sites. Same as DCFlushRange minus the trailing sync; on SH-4 just alias to the flush. |
| `DCStoreRangeNoSync` | `void DCStoreRangeNoSync(void* addr, u32 len)` | pc_os.c | rewrite-for-KOS | ★ 25 call sites — the most-used cache op in the codebase. All in jaudio + JKRAram paths. |
| `DCZeroRange` | `void DCZeroRange(void* addr, u32 len)` | pc_os.c | rewrite-for-KOS | PC does memset. On SH-4 the real op allocates cache lines without reading memory; memset is a correct (slower) substitute. |
| `ICFlashInvalidate` | `void ICFlashInvalidate(void)` | pc_os.c | rewrite-for-KOS |  |
| `ICInvalidateRange` | `void ICInvalidateRange(void* addr, u32 len)` | pc_os.c | rewrite-for-KOS | 0 call sites in compiled code. Keep as no-op. |
| `LCEnable` ✱ | `void LCEnable(void)` | pc_os.c | drop | GC locked-cache. 0 call sites. Drop. |
| `LCDisable` | `void LCDisable(void)` | pc_os.c | drop | GC locked-cache. 0 call sites. Drop. |
| `osWritebackDCache` | `void osWritebackDCache(void* vaddr, u32 nbytes)` | pc_stubs.c | rewrite-for-KOS | ★ REAL on SH-4. 8 call sites in compiled libultra-flavoured game code; currently a no-op. Same treatment as DCStoreRange. |

### OS — address translation

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `OSPhysicalToCached` | `void* OSPhysicalToCached(u32 paddr)` | pc_os.c | rewrite-for-KOS | ★ LANDMINE. PC maps phys 0 -> arena base, so JKRHeap's `start = OSPhysicalToCached(0)` and `*(u32*)(start+0x28)` land inside the arena. On DC the natural mapping is phys 0 -> 0x8C000000; then +0x28 is inside KOS's own low memory. Either keep the PC trick (phys 0 == arena base) or explicitly reserve/write the 0x28 word. |
| `OSPhysicalToUncached` | `void* OSPhysicalToUncached(u32 paddr)` | pc_os.c | rewrite-for-KOS |  |
| `OSCachedToPhysical` | `u32 OSCachedToPhysical(void* caddr)` | pc_os.c | rewrite-for-KOS | Inverse of the above; used by emu64/ARAM address plumbing. Keep the two consistent. |
| `OSUncachedToPhysical` | `u32 OSUncachedToPhysical(void* ucaddr)` | pc_os.c | rewrite-for-KOS |  |
| `OSCachedToUncached` | `void* OSCachedToUncached(void* caddr)` | pc_os.c | rewrite-for-KOS | Identity on PC. On SH-4 this is a REAL operation: P1 (0x8Cxxxxxx) -> P2 (0xACxxxxxx). Any buffer handed to PVR/AICA/DMA must use the P2 alias or be flushed. |
| `OSUncachedToCached` | `void* OSUncachedToCached(void* ucaddr)` | pc_os.c | rewrite-for-KOS | SH-4: clear bit 0x20000000. Real operation, not identity. |

### OS — threads, queues, sync

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `osCreateMesgQueue` | `void osCreateMesgQueue(void* mq, void* buf, int count)` | pc_os.c | port-as-is |  |
| `osSendMesg` | `int osSendMesg(void* mq, void* msg, int flags)` | pc_os.c | port-as-is |  |
| `osRecvMesg` | `int osRecvMesg(void* mq, void** msg, int flags)` | pc_os.c | port-as-is |  |
| `osCreateThread2` | `void osCreateThread2(void* thread, int id, void (*entry)(void*), void* arg, void* stack, int stack_size, int priority)` | pc_os.c | rewrite-for-KOS |  |
| `osStartThread` | `void osStartThread(void* thread)` | pc_os.c | rewrite-for-KOS |  |
| `osSetThreadPri` | `void osSetThreadPri(void* thread, int pri)` | pc_os.c | rewrite-for-KOS |  |
| `OSGetCurrentThread` | `void* OSGetCurrentThread(void)` | pc_os.c | rewrite-for-KOS | ★ boot.c dereferences the returned pointer's stackBase/stackEnd fields (OSThread layout) to paint the stack with 0xFD. Returning a bogus pointer crashes at boot. PC returns a 512-byte zeroed dummy. |
| `OSGetStackPointer` | `void* OSGetStackPointer(void)` | pc_os.c | rewrite-for-KOS | PC returns &a_static — only used relative to the above; any in-arena address works. |
| `OSDisableInterrupts` | `BOOL OSDisableInterrupts(void)` | pc_os.c | rewrite-for-KOS | PC returns 0 always. If DC uses real KOS threads for audio, these must become irq_disable()/irq_restore() or the jaudio queues race. |
| `OSEnableInterrupts` | `BOOL OSEnableInterrupts(void)` | pc_os.c | rewrite-for-KOS |  |
| `OSRestoreInterrupts` | `BOOL OSRestoreInterrupts(BOOL level)` | pc_os.c | rewrite-for-KOS |  |
| `msleep` | `void msleep(int ms)` | pc_os.c | rewrite-for-KOS | boot.c sleeps 2500 ms during sound_initial(); with a real DC boot this is a visible dead wait — candidate to shorten. |
| `OSInitMutex` | `void OSInitMutex(OSMutex* mutex)` | pc_os.c | rewrite-for-KOS |  |
| `OSLockMutex` | `void OSLockMutex(OSMutex* mutex)` | pc_os.c | rewrite-for-KOS |  |
| `OSUnlockMutex` | `void OSUnlockMutex(OSMutex* mutex)` | pc_os.c | rewrite-for-KOS |  |
| `OSTryLockMutex` | `BOOL OSTryLockMutex(OSMutex* mutex)` | pc_os.c | rewrite-for-KOS |  |
| `OSInitMessageQueue` | `void OSInitMessageQueue(void* queue, void** msgArray, int msgCount)` | pc_os.c | rewrite-for-KOS | ★ Layout-critical: the struct must match dolphin/os/OSMessage.h exactly (16 bytes of thread queues, then msgArray/msgCount/firstIndex/usedCount). The game allocates these structs itself. |
| `OSSendMessage` | `BOOL OSSendMessage(void* queue, void* msg, int flags)` | pc_os.c | rewrite-for-KOS | PC version is NON-BLOCKING and returns FALSE when full (real Dolphin blocks when flags==OS_MESSAGE_BLOCK). jaudio depends on the non-blocking behavior in the TARGET_PC path. |
| `OSReceiveMessage` | `BOOL OSReceiveMessage(void* queue, void** msgPtr, int flags)` | pc_os.c | rewrite-for-KOS |  |
| `OSJamMessage` | `BOOL OSJamMessage(void* queue, void* msg, int flags)` | pc_os.c | rewrite-for-KOS |  |
| `OSCreateThread` | `BOOL OSCreateThread(void* thread, void* (*func)(void*), void* param, void* stack, u32 stackSize, OSPriority priority, u16 attr)` | pc_stubs.c | rewrite-for-KOS | ★ Called by JKRThread/JKRDecomp and by jaudio's GC path (which is #ifdef'd out for TARGET_PC). PC returns FALSE and never runs the thread. If DC uses real KOS threads for audio, this stub has to become real. |
| `OSCancelThread` | `void OSCancelThread(void* thread)` | pc_stubs.c | rewrite-for-KOS |  |
| `OSDetachThread` | `void OSDetachThread(void* thread)` | pc_stubs.c | rewrite-for-KOS |  |
| `OSResumeThread` | `s32 OSResumeThread(void* thread)` | pc_stubs.c | rewrite-for-KOS |  |
| `OSSuspendThread` | `s32 OSSuspendThread(void* thread)` | pc_stubs.c | rewrite-for-KOS | PC no-op; JKRDecomp assumes a suspended decompression thread it can resume. |
| `OSIsThreadTerminated` | `BOOL OSIsThreadTerminated(void* thread)` | pc_stubs.c | rewrite-for-KOS |  |
| `OSEnableScheduler` | `s32 OSEnableScheduler(void)` | pc_stubs.c | rewrite-for-KOS |  |
| `OSYieldThread` | `void OSYieldThread(void)` | pc_stubs.c | rewrite-for-KOS |  |
| `OSCheckActiveThreads` | `long OSCheckActiveThreads(void)` | pc_stubs.c | rewrite-for-KOS |  |
| `OSFillFPUContext` | `void OSFillFPUContext(void* context)` | pc_stubs.c | rewrite-for-KOS |  |

### OS — init / misc

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `OSInitFastCast` | `void OSInitFastCast(void)` | pc_misc.c | port-as-is |  |
| `__OSSetupFPU` ✱ | `void __OSSetupFPU(void)` | pc_misc.c | port-as-is |  |
| `__sync` | `void __sync(void)` | pc_misc.c | port-as-is |  |
| `__isync` ✱ | `void __isync(void)` | pc_misc.c | port-as-is |  |
| `OSGetSaveRegion` | `void OSGetSaveRegion(void** start, void** end)` | pc_os.c | port-as-is |  |
| `OSInit` | `void OSInit(void)` | pc_os.c | rewrite-for-KOS |  |
| `OSInitAlarm` | `void OSInitAlarm(void)` | pc_os.c | port-as-is |  |
| `__osInitialize_common` | `void __osInitialize_common(void)` | pc_os.c | port-as-is |  |
| `__OSPSInit` | `void __OSPSInit(void)` | pc_os.c | port-as-is |  |
| `__OSFPRInit` | `void __OSFPRInit(void)` | pc_os.c | port-as-is |  |
| `__OSCacheInit` | `void __OSCacheInit(void)` | pc_os.c | port-as-is |  |
| `OSGetConsoleType` | `u32 OSGetConsoleType(void)` | pc_os.c | port-as-is |  |
| `OSSetAlarm` | `void OSSetAlarm(OSAlarm* alarm, s64 tick, void (*callback)(OSAlarm*, void*))` | pc_os.c | port-as-is |  |
| `OSSetPeriodicAlarm` | `void OSSetPeriodicAlarm(OSAlarm* alarm, s64 start, s64 period, void (*callback)(OSAlarm*, void*))` | pc_os.c | port-as-is |  |
| `OSCancelAlarm` | `void OSCancelAlarm(OSAlarm* alarm)` | pc_os.c | port-as-is |  |
| `OSCreateAlarm` | `void OSCreateAlarm(OSAlarm* alarm)` | pc_os.c | port-as-is |  |
| `ReconfigBATs` | `void ReconfigBATs(void)` | pc_os.c | port-as-is |  |
| `OSSetStringTable` | `void OSSetStringTable(void* table)` | pc_os.c | port-as-is |  |
| `__OSLockSramEx` | `OSSramEx* __OSLockSramEx(void)` | pc_os.c | port-as-is |  |
| `__OSUnlockSramEx` | `void __OSUnlockSramEx(BOOL commit)` | pc_os.c | port-as-is |  |
| `OSClearContext` | `void OSClearContext(OSContext* ctx)` | pc_os.c | port-as-is |  |
| `__OSSetExceptionHandler` | `void* __OSSetExceptionHandler(u8 type, void* handler)` | pc_os.c | rewrite-for-KOS | ★ Use this seam to install the SH-4 unaligned-access / TLB exception handler that PLAN §3.2 needs for -O2 triage. |

### OS — diagnostics

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `__OSUnhandledException` | `void __OSUnhandledException(u8 type, void* ctx, u32 dsisr, u32 dar)` | pc_misc.c | rewrite-for-KOS | PC prints and returns. DC should dump PC + faulting address, matching the base port's SIGBUS recovery pattern. |
| `OSPanic` | `void OSPanic(const char* file, int line, const char* msg, ...)` | pc_os.c | rewrite-for-KOS |  |
| `OSReport` | `void OSReport(const char* fmt, ...)` | pc_os.c | rewrite-for-KOS | Every OSReport goes through here; gated on g_pc_verbose. On DC route to KOS dbgio (dcload/serial) and keep it OFF by default — serial at 57600 baud will destroy frame time. |
| `OSVReport` | `void OSVReport(const char* fmt, va_list list)` | pc_os.c | rewrite-for-KOS |  |
| `OSReportDisable` | `void OSReportDisable(void)` | pc_os.c | rewrite-for-KOS |  |

### OS — reset / shutdown

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `OSRegisterResetFunction` | `void OSRegisterResetFunction(OSResetFunctionInfo* info)` | pc_misc.c | drop |  |
| `OSUnregisterResetFunction` | `void OSUnregisterResetFunction(OSResetFunctionInfo* info)` | pc_misc.c | drop |  |
| `OSGetResetSwitchState` | `BOOL OSGetResetSwitchState(void)` | pc_os.c | rewrite-for-KOS |  |
| `OSResetSystem` | `void OSResetSystem(int reset, u32 resetCode, BOOL forceMenu)` | pc_os.c | rewrite-for-KOS | PC sets g_pc_running=0. DC: arch_exit()/arch_reboot(). |
| `OSGetResetCode` | `u32 OSGetResetCode(void)` | pc_os.c | rewrite-for-KOS |  |
| `OSChangeBootMode` | `void OSChangeBootMode(u32 mode)` | pc_os.c | rewrite-for-KOS |  |
| `osShutdownStart` | `void osShutdownStart(int type)` | pc_os.c | rewrite-for-KOS |  |

### OS — REL module loader

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `OSLink` | `BOOL OSLink(void* info, void* bss)` | pc_misc.c | stub-and-log | Returns TRUE without doing anything — the PC build links foresta.rel's code in statically instead of relocating it. Same approach needed for DC (or a real REL loader); 1 call site in boot.c. |
| `OSUnlink` | `BOOL OSUnlink(void* info)` | pc_misc.c | stub-and-log |  |

### libc64 malloc-arena shims

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `MallocInit` | `void MallocInit(void* base, unsigned long size)` | pc_misc.c | port-as-is | libc64/malloc.c is excluded from the build; these wrap the game's arena bookkeeping. jsyswrap.cpp calls MallocInit(gameheap_base, gameheap_len) where gameheap = systemHeap free - 0x10000. |
| `MallocCleanup` | `void MallocCleanup(void)` | pc_misc.c | port-as-is |  |
| `MallocIsInitalized` | `int MallocIsInitalized(void)` | pc_misc.c | port-as-is |  |
| `GetFreeArena` | `void GetFreeArena(unsigned long* max, unsigned long* free_size, unsigned long* alloc)` | pc_misc.c | port-as-is | Reports the malloc arena; used by DisplayArena in the fault handler. |
| `DisplayArena` | `void DisplayArena(void)` | pc_misc.c | port-as-is |  |
| `CheckArena` | `int CheckArena(void)` | pc_misc.c | port-as-is |  |

### N64 fixed-point trig

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `sins` | `short sins(unsigned short angle)` | pc_misc.c | port-as-is | N64 fixed-point sin over a u16 angle. Uses double sin() per call — on SH-4 replace with a 16-bit LUT. |
| `coss` | `short coss(unsigned short angle)` | pc_misc.c | port-as-is |  |

### PPC intrinsic shims

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `PPCMfmsr` | `u32 PPCMfmsr(void)` | pc_misc.c | stub-and-log | 2 live call sites (JUTException.cpp masks interrupts via MSR). Needs an SH-4 SR equivalent or the exception path changes behavior. |
| `PPCMtmsr` | `void PPCMtmsr(u32 msr)` | pc_misc.c | stub-and-log |  |
| `PPCMfhid0` | `u32 PPCMfhid0(void)` | pc_misc.c | drop |  |
| `PPCMthid0` | `void PPCMthid0(u32 hid0)` | pc_misc.c | drop |  |
| `PPCMfhid2` | `u32 PPCMfhid2(void)` | pc_misc.c | drop |  |
| `PPCMthid2` | `void PPCMthid2(u32 hid2)` | pc_misc.c | drop |  |
| `PPCHalt` | `void PPCHalt(void)` | pc_misc.c | drop |  |
| `PPCMfl2cr` | `u32 PPCMfl2cr(void)` | pc_misc.c | drop |  |
| `PPCMtl2cr` | `void PPCMtl2cr(u32 val)` | pc_misc.c | drop |  |
| `PPCMtdec` | `void PPCMtdec(u32 val)` | pc_misc.c | drop |  |
| `PPCSync` | `void PPCSync(void)` | pc_misc.c | stub-and-log | 2 live call sites (emu64.c:5912, dspdriver.c:373) — memory barriers. On SH-4 these should be real (`__sync_synchronize()` / OCBWB), not no-ops, if any DMA is in flight. |
| `PPCMtmmcr0` ✱ | `void PPCMtmmcr0(u32 val)` | pc_misc.c | drop |  |
| `PPCMtmmcr1` ✱ | `void PPCMtmmcr1(u32 val)` | pc_misc.c | drop |  |
| `PPCMtpmc1` ✱ | `void PPCMtpmc1(u32 val)` | pc_misc.c | drop |  |
| `PPCMtpmc2` ✱ | `void PPCMtpmc2(u32 val)` | pc_misc.c | drop |  |
| `PPCMtpmc3` ✱ | `void PPCMtpmc3(u32 val)` | pc_misc.c | drop |  |
| `PPCMtpmc4` ✱ | `void PPCMtpmc4(u32 val)` | pc_misc.c | drop |  |
| `PPCMfpmc1` ✱ | `u32 PPCMfpmc1(void)` | pc_misc.c | drop |  |
| `PPCMfpmc2` ✱ | `u32 PPCMfpmc2(void)` | pc_misc.c | drop |  |
| `PPCMfpmc3` ✱ | `u32 PPCMfpmc3(void)` | pc_misc.c | drop |  |
| `PPCMfpmc4` ✱ | `u32 PPCMfpmc4(void)` | pc_misc.c | drop |  |

### EXI / SI / debugger shims

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `EXILock` | `BOOL EXILock(s32 chan, u32 dev, void* unlockCallback)` | pc_misc.c | drop |  |
| `EXIUnlock` | `BOOL EXIUnlock(s32 chan)` | pc_misc.c | drop |  |
| `EXISelect` | `BOOL EXISelect(s32 chan, u32 dev, u32 freq)` | pc_misc.c | drop |  |
| `EXIDeselect` | `BOOL EXIDeselect(s32 chan)` | pc_misc.c | drop |  |
| `EXIImm` | `BOOL EXIImm(s32 chan, void* data, s32 len, u32 type, void* callback)` | pc_misc.c | drop |  |
| `EXIDma` | `BOOL EXIDma(s32 chan, void* data, s32 len, u32 type, void* callback)` | pc_misc.c | drop |  |
| `EXISync` | `BOOL EXISync(s32 chan)` | pc_misc.c | drop |  |
| `EXIInit` | `void EXIInit(void)` | pc_misc.c | drop |  |
| `EXIAttach` | `BOOL EXIAttach(s32 chan, void* extCallback)` | pc_misc.c | drop |  |
| `EXIDetach` | `BOOL EXIDetach(s32 chan)` | pc_misc.c | drop |  |
| `EXIProbe` | `BOOL EXIProbe(s32 chan)` | pc_misc.c | drop |  |
| `EXIProbeEx` | `BOOL EXIProbeEx(s32 chan)` | pc_misc.c | drop |  |
| `EXIGetID` | `s32 EXIGetID(s32 chan, u32 dev, u32* id)` | pc_misc.c | drop |  |
| `EXISetExiCallback` | `void EXISetExiCallback(s32 chan, void* cb)` | pc_misc.c | drop |  |
| `SIInit` | `void SIInit(void)` | pc_misc.c | drop |  |
| `SIGetType` | `u32 SIGetType(s32 chan)` | pc_misc.c | drop |  |
| `SIGetTypeAsync` | `u32 SIGetTypeAsync(s32 chan, void* callback)` | pc_misc.c | drop |  |
| `SITransfer` | `BOOL SITransfer(s32 chan, void* output, u32 outputLen, void* input, u32 inputLen, void* callback, s64 time)` | pc_misc.c | drop |  |
| `SISetXY` | `u32 SISetXY(u32 x, u32 y)` | pc_misc.c | drop |  |
| `SIEnablePolling` | `u32 SIEnablePolling(u32 poll)` | pc_misc.c | drop |  |
| `SIDisablePolling` | `u32 SIDisablePolling(u32 poll)` | pc_misc.c | drop |  |
| `SISetSamplingRate` | `void SISetSamplingRate(u32 rate)` | pc_misc.c | drop |  |
| `SIIsChanBusy` | `BOOL SIIsChanBusy(s32 chan)` | pc_misc.c | drop |  |
| `SIRefreshSamplingRate` | `void SIRefreshSamplingRate(void)` | pc_misc.c | drop |  |
| `SIRegisterPollingHandler` | `BOOL SIRegisterPollingHandler(void* handler)` | pc_misc.c | drop |  |
| `SIUnregisterPollingHandler` | `BOOL SIUnregisterPollingHandler(void* handler)` | pc_misc.c | drop |  |
| `DBInit` | `void DBInit(void)` | pc_misc.c | drop |  |
| `DBIsDebuggerPresent` | `BOOL DBIsDebuggerPresent(void)` | pc_misc.c | drop |  |
| `DBPrintf` | `void DBPrintf(const char* fmt, ...)` | pc_misc.c | drop |  |
| `InitMetroTRK` | `void InitMetroTRK(void)` | pc_misc.c | port-as-is |  |
| `InitMetroTRK_BBA` | `void InitMetroTRK_BBA(void)` | pc_misc.c | port-as-is |  |

### libc shims

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `bzero` | `void bzero(void* s, unsigned int n)` | pc_misc.c | drop |  |
| `bcopy` | `void bcopy(const void* src, void* dst, unsigned int n)` | pc_misc.c | drop |  |

### MTX / GU math (PPC paired-singles replacement)

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `PSMTXIdentity` | `void PSMTXIdentity(MtxP m)` | pc_mtx.c | port-as-is |  |
| `C_MTXIdentity` | `void C_MTXIdentity(MtxP m)` | pc_mtx.c | port-as-is |  |
| `PSMTXCopy` | `void PSMTXCopy(const MtxP src, MtxP dst)` | pc_mtx.c | port-as-is |  |
| `PSMTXConcat` | `void PSMTXConcat(const MtxP a, const MtxP b, MtxP result)` | pc_mtx.c | port-as-is | PPC paired-singles replaced with scalar C. #1 candidate for SH-4 FTRV (4x4 matrix multiply in one instruction). |
| `PSMTXInverse` | `void PSMTXInverse(const MtxP src, MtxP inv)` | pc_mtx.c | port-as-is | Scalar; not hot. |
| `PSMTXMultVec` | `void PSMTXMultVec(const MtxP m, const Vec* src, Vec* dst)` | pc_mtx.c | port-as-is |  |
| `PSMTXMultVecSR` | `void PSMTXMultVecSR(const MtxP m, const Vec* src, Vec* dst)` | pc_mtx.c | port-as-is |  |
| `PSMTXMultVecArray` | `void PSMTXMultVecArray(const MtxP m, const Vec* srcBase, Vec* dstBase, u32 count)` | pc_mtx.c | port-as-is | Bulk vector transform — SH-4 FTRV loop, ~32 vertices per block (dca3 pattern). |
| `PSMTXScale` | `void PSMTXScale(MtxP m, f32 sx, f32 sy, f32 sz)` | pc_mtx.c | port-as-is |  |
| `PSMTXTrans` | `void PSMTXTrans(MtxP m, f32 tx, f32 ty, f32 tz)` | pc_mtx.c | port-as-is |  |
| `PSMTXTransApply` | `void PSMTXTransApply(const MtxP src, MtxP dst, f32 tx, f32 ty, f32 tz)` | pc_mtx.c | port-as-is |  |
| `PSMTXScaleApply` | `void PSMTXScaleApply(const MtxP src, MtxP dst, f32 sx, f32 sy, f32 sz)` | pc_mtx.c | port-as-is |  |
| `PSVECNormalize` | `void PSVECNormalize(const Vec* src, Vec* dst)` | pc_mtx.c | port-as-is |  |
| `PSVECCrossProduct` | `void PSVECCrossProduct(const Vec* a, const Vec* b, Vec* dst)` | pc_mtx.c | port-as-is |  |
| `PSVECDotProduct` | `f32 PSVECDotProduct(const Vec* a, const Vec* b)` | pc_mtx.c | port-as-is | SH-4 FIPR (4-way dot product) candidate. |
| `PSVECMag` | `f32 PSVECMag(const Vec* v)` | pc_mtx.c | port-as-is |  |
| `C_MTXFrustum` | `void C_MTXFrustum(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f)` | pc_mtx.c | port-as-is |  |
| `C_MTXPerspective` | `void C_MTXPerspective(Mtx44 m, f32 fovY, f32 aspect, f32 n, f32 f)` | pc_mtx.c | port-as-is | Builds a GC-convention projection (z in [-1,0]); PVR wants its own W-buffer convention — check this before debugging 'everything is inside out'. |
| `C_MTXOrtho` | `void C_MTXOrtho(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f)` | pc_mtx.c | port-as-is |  |
| `C_MTXLookAt` | `void C_MTXLookAt(MtxP m, const Vec* camPos, const Vec* camUp, const Vec* target)` | pc_mtx.c | port-as-is |  |
| `C_MTXLightPerspective` | `void C_MTXLightPerspective(MtxP m, f32 fovY, f32 aspect, f32 scaleS, f32 scaleT, f32 transS, f32 transT)` | pc_mtx.c | port-as-is |  |
| `C_MTXLightOrtho` | `void C_MTXLightOrtho(MtxP m, f32 t, f32 b, f32 l, f32 r, f32 scaleS, f32 scaleT, f32 transS, f32 transT)` | pc_mtx.c | port-as-is |  |
| `guMtxIdentF` | `void guMtxIdentF(float mf[4][4])` | pc_mtx.c | port-as-is |  |
| `guMtxF2L` | `void guMtxF2L(float mf[4][4], Mtx* m)` | pc_mtx.c | port-as-is |  |
| `guMtxIdent` | `void guMtxIdent(Mtx* m)` | pc_mtx.c | port-as-is |  |
| `guOrthoF` | `void guOrthoF(float mf[4][4], float l, float r, float b, float t, float n, float f, float scale)` | pc_mtx.c | port-as-is |  |
| `guOrtho` | `void guOrtho(Mtx* m, float l, float r, float b, float t, float n, float f, float scale)` | pc_mtx.c | port-as-is |  |
| `guPerspectiveF` | `void guPerspectiveF(float mf[4][4], u16* perspNorm, float fovy, float aspect, float near, float far, float scale)` | pc_mtx.c | port-as-is |  |
| `guPerspective` | `void guPerspective(Mtx* m, u16* perspNorm, float fovy, float aspect, float near, float far, float scale)` | pc_mtx.c | port-as-is |  |
| `guLookAtF` | `void guLookAtF(float mf[4][4], float xEye, float yEye, float zEye, float xAt, float yAt, float zAt, float xUp, float yUp, float zUp)` | pc_mtx.c | port-as-is |  |
| `guLookAt` | `void guLookAt(Mtx* m, float xEye, float yEye, float zEye, float xAt, float yAt, float zAt, float xUp, float yUp, float zUp)` | pc_mtx.c | port-as-is |  |
| `guLookAtHilite` | `void guLookAtHilite(Mtx* m, void* lv, void* hv, float xEye, float yEye, float zEye, float xAt, float yAt, float zAt, float xUp, float yUp, float zUp, float xl1, float yl1, float zl1, float xl2, float yl2, float zl2, int twidth, int theight)` | pc_mtx.c | port-as-is |  |
| `guScale` | `void guScale(Mtx* m, float x, float y, float z)` | pc_mtx.c | port-as-is |  |
| `guTranslate` | `void guTranslate(Mtx* m, float x, float y, float z)` | pc_mtx.c | port-as-is |  |
| `guRotateF` | `void guRotateF(float mf[4][4], float a, float x, float y, float z)` | pc_mtx.c | port-as-is |  |
| `guRotate` | `void guRotate(Mtx* m, float a, float x, float y, float z)` | pc_mtx.c | port-as-is |  |
| `guNormalize` | `void guNormalize(float* x, float* y, float* z)` | pc_mtx.c | port-as-is |  |

### GX — init / sync

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `GXInit` | `void* GXInit(void* base, u32 size)` | pc_gx.c | rewrite-for-KOS | PC returns `base` unchanged and ignores the FIFO. Called from JUTGraphFifo.cpp with a JKRHeap-allocated 0x10001-byte buffer (JW_Init sets FifoBufSize). On DC that allocation can be reclaimed. |
| `GXSetMisc` | `void GXSetMisc(u32 token, u32 val)` | pc_gx.c | rewrite-for-KOS |  |
| `GXFlush` | `void GXFlush(void)` | pc_gx.c | rewrite-for-KOS | Maps to nothing on a deferred tile renderer; leave as a no-op. |
| `GXResetWriteGatherPipe` | `void GXResetWriteGatherPipe(void)` | pc_gx.c | rewrite-for-KOS |  |
| `GXAbortFrame` | `void GXAbortFrame(void)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetDrawSync` | `void GXSetDrawSync(u16 token)` | pc_gx.c | rewrite-for-KOS |  |
| `GXReadDrawSync` | `u16 GXReadDrawSync(void)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetDrawDone` | `void GXSetDrawDone(void)` | pc_gx.c | rewrite-for-KOS |  |
| `GXWaitDrawDone` | `void GXWaitDrawDone(void)` | pc_gx.c | rewrite-for-KOS |  |
| `GXDrawDone` | `void GXDrawDone(void)` | pc_gx.c | rewrite-for-KOS | No-op on PC; on DC this is a real pvr_wait_ready()/scene sync point. |
| `GXPixModeSync` | `void GXPixModeSync(void)` | pc_gx.c | rewrite-for-KOS |  |
| `GXTexModeSync` | `void GXTexModeSync(void)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetDrawSyncCallback` | `void* GXSetDrawSyncCallback(void* cb)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetDrawDoneCallback` | `void* GXSetDrawDoneCallback(void* cb)` | pc_gx.c | rewrite-for-KOS |  |

### GX — vertex submission (immediate mode)

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `GXBegin` | `void GXBegin(u32 primitive, u32 vtxfmt, u16 nverts)` | pc_gx.c | rewrite-for-KOS | ★ Batching heart. Merges consecutive complete batches when dirty==0 and prim is QUADS/TRIANGLES; converts TRIANGLESTRIP/FAN to independent triangles up front. `nverts` is the DECLARED count — emu64 never calls GXEnd, so completion is detected by counting. |
| `GXEnd` | `void GXEnd(void)` | pc_gx.c | rewrite-for-KOS | ★ emu64 OMITS GXEnd. Never rely on it; pc_gx_flush_if_begin_complete() is what actually terminates a batch. |
| `GXPosition3f32` | `void GXPosition3f32(f32 x, f32 y, f32 z)` | pc_gx.c | rewrite-for-KOS | ★ Deferred-commit state machine: a position call COMMITS the previous vertex, then resets normal/color1/texcoords but CARRIES COLOR0 FORWARD. Any DC rewrite must preserve the carry-forward or vertex colors break. |
| `GXPosition3u16` | `void GXPosition3u16(u16 x, u16 y, u16 z)` | pc_gx.c | rewrite-for-KOS |  |
| `GXPosition3s16` | `void GXPosition3s16(s16 x, s16 y, s16 z)` | pc_gx.c | rewrite-for-KOS |  |
| `GXPosition3u8` | `void GXPosition3u8(u8 x, u8 y, u8 z)` | pc_gx.c | rewrite-for-KOS |  |
| `GXPosition3s8` | `void GXPosition3s8(s8 x, s8 y, s8 z)` | pc_gx.c | rewrite-for-KOS |  |
| `GXPosition2f32` | `void GXPosition2f32(f32 x, f32 y)` | pc_gx.c | rewrite-for-KOS |  |
| `GXPosition2u16` | `void GXPosition2u16(u16 x, u16 y)` | pc_gx.c | rewrite-for-KOS |  |
| `GXPosition2s16` | `void GXPosition2s16(s16 x, s16 y)` | pc_gx.c | rewrite-for-KOS |  |
| `GXPosition2u8` | `void GXPosition2u8(u8 x, u8 y)` | pc_gx.c | rewrite-for-KOS |  |
| `GXPosition2s8` | `void GXPosition2s8(s8 x, s8 y)` | pc_gx.c | rewrite-for-KOS |  |
| `GXPosition1x16` | `void GXPosition1x16(u16 index)` | pc_gx.c | rewrite-for-KOS | Indexed attribute fetch from GXSetArray base + stride. Assumes f32[3] source. Indexed positions/normals are how the game submits most static geometry. |
| `GXPosition1x8` | `void GXPosition1x8(u8 index)` | pc_gx.c | rewrite-for-KOS |  |
| `GXNormal3f32` | `void GXNormal3f32(f32 x, f32 y, f32 z)` | pc_gx.c | rewrite-for-KOS |  |
| `GXNormal3s16` | `void GXNormal3s16(s16 x, s16 y, s16 z)` | pc_gx.c | rewrite-for-KOS |  |
| `GXNormal3s8` | `void GXNormal3s8(s8 x, s8 y, s8 z)` | pc_gx.c | rewrite-for-KOS |  |
| `GXNormal1x16` | `void GXNormal1x16(u16 index)` | pc_gx.c | rewrite-for-KOS |  |
| `GXNormal1x8` | `void GXNormal1x8(u8 index)` | pc_gx.c | rewrite-for-KOS |  |
| `GXColor4u8` | `void GXColor4u8(u8 r, u8 g, u8 b, u8 a)` | pc_gx.c | rewrite-for-KOS |  |
| `GXColor3u8` | `void GXColor3u8(u8 r, u8 g, u8 b)` | pc_gx.c | rewrite-for-KOS |  |
| `GXColor1u32` | `void GXColor1u32(u32 clr)` | pc_gx.c | rewrite-for-KOS | Packed RGBA8; several variants funnel to GXColor4u8. |
| `GXColor1u16` | `void GXColor1u16(u16 clr)` | pc_gx.c | rewrite-for-KOS |  |
| `GXColor1x16` | `void GXColor1x16(u16 index)` | pc_gx.c | rewrite-for-KOS |  |
| `GXColor1x8` | `void GXColor1x8(u8 index)` | pc_gx.c | rewrite-for-KOS |  |
| `GXColor4f32` | `void GXColor4f32(float r, float g, float b, float a)` | pc_gx.c | rewrite-for-KOS |  |
| `GXTexCoord2f32` | `void GXTexCoord2f32(f32 s, f32 t)` | pc_gx.c | rewrite-for-KOS |  |
| `GXTexCoord2u16` | `void GXTexCoord2u16(u16 s, u16 t)` | pc_gx.c | rewrite-for-KOS |  |
| `GXTexCoord2s16` | `void GXTexCoord2s16(s16 s, s16 t)` | pc_gx.c | rewrite-for-KOS |  |
| `GXTexCoord2u8` | `void GXTexCoord2u8(u8 s, u8 t)` | pc_gx.c | rewrite-for-KOS |  |
| `GXTexCoord2s8` | `void GXTexCoord2s8(s8 s, s8 t)` | pc_gx.c | rewrite-for-KOS |  |
| `GXTexCoord1f32` | `void GXTexCoord1f32(f32 s, f32 t)` | pc_gx.c | rewrite-for-KOS |  |
| `GXTexCoord1u16` | `void GXTexCoord1u16(u16 s, u16 t)` | pc_gx.c | rewrite-for-KOS |  |
| `GXTexCoord1s16` | `void GXTexCoord1s16(s16 s, s16 t)` | pc_gx.c | rewrite-for-KOS |  |
| `GXTexCoord1u8` | `void GXTexCoord1u8(u8 s, u8 t)` | pc_gx.c | rewrite-for-KOS |  |
| `GXTexCoord1s8` | `void GXTexCoord1s8(s8 s, s8 t)` | pc_gx.c | rewrite-for-KOS |  |
| `GXTexCoord1x16` | `void GXTexCoord1x16(u16 index)` | pc_gx.c | rewrite-for-KOS |  |
| `GXTexCoord1x8` | `void GXTexCoord1x8(u8 index)` | pc_gx.c | rewrite-for-KOS |  |

### GX — vertex descriptor / indexed arrays

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `GXSetVtxDesc` | `void GXSetVtxDesc(u32 attr, u32 type)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetVtxDescv` | `void GXSetVtxDescv(const void* list)` | pc_gx.c | rewrite-for-KOS |  |
| `GXClearVtxDesc` | `void GXClearVtxDesc(void)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetVtxAttrFmt` | `void GXSetVtxAttrFmt(u32 vtxfmt, u32 attr, u32 cnt, u32 type, u8 frac)` | pc_gx.c | rewrite-for-KOS | Recorded but the PC path always builds the same 48-byte PCGXVertex. On DC the record shrinks to a PVR-native 32-byte vertex. |
| `GXSetArray` | `void GXSetArray(u32 attr, const void* data, u32 size, u8 stride)` | pc_gx.c | rewrite-for-KOS | ★ Stores a raw pointer + stride per attribute; the pointer is a GameCube-era 32-bit address the game computed. On DC this must survive the seg2k0 pointer heuristic (PLAN §11.6). |
| `GXInvalidateVtxCache` | `void GXInvalidateVtxCache(void)` | pc_gx.c | rewrite-for-KOS |  |
| `GXGetVtxAttrFmt` | `void GXGetVtxAttrFmt(u32 idx, u32 attr, u32* compCnt, u32* compType, u8* shift)` | pc_gx.c | rewrite-for-KOS |  |

### GX — transform / viewport / scissor

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `GXSetProjection` | `void GXSetProjection(const void* mtx, u32 type)` | pc_gx.c | rewrite-for-KOS | GC projection matrices are 4x4 (GX_PERSPECTIVE) or 3x4 (GX_ORTHOGRAPHIC) with a different memory layout — check the `type` argument. |
| `GXLoadPosMtxImm` | `void GXLoadPosMtxImm(const void* mtx, u32 id)` | pc_gx.c | rewrite-for-KOS | 3x4 row-major modelview into slot id (0..9 used). 41% of all batch breaks are modelview loads — the single biggest batching obstacle. |
| `GXLoadNrmMtxImm` | `void GXLoadNrmMtxImm(const void* mtx, u32 id)` | pc_gx.c | rewrite-for-KOS |  |
| `GXLoadTexMtxImm` | `void GXLoadTexMtxImm(const void* mtx, u32 id, u32 type)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetCurrentMtx` | `void GXSetCurrentMtx(u32 id)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetViewport` | `void GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz)` | pc_gx.c | rewrite-for-KOS | Shadowed: re-setting an identical viewport skips both flush and the GL call. Keep that dedup on DC. |
| `GXSetViewportJitter` | `void GXSetViewportJitter(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz, u32 field)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetScissor` | `void GXSetScissor(u32 left, u32 top, u32 wd, u32 ht)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetScissorBoxOffset` | `void GXSetScissorBoxOffset(s32 x, s32 y)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetClipMode` | `void GXSetClipMode(u32 mode)` | pc_gx.c | rewrite-for-KOS |  |
| `GXGetProjectionv` | `void GXGetProjectionv(f32* p)` | pc_gx.c | rewrite-for-KOS |  |

### GX — TEV / indirect

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `GXSetNumTevStages` | `void GXSetNumTevStages(u8 nStages)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetTevOp` | `void GXSetTevOp(u32 stage, u32 mode)` | pc_gx.c | rewrite-for-KOS | One of 101 harvested TEV configurations (kb/renderer.md). Max 3 stages (PC_GX_MAX_TEV_STAGES). This is the surface tev_map.md must classify. |
| `GXSetTevColorIn` | `void GXSetTevColorIn(u32 stage, u32 a, u32 b, u32 c, u32 d)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetTevAlphaIn` | `void GXSetTevAlphaIn(u32 stage, u32 a, u32 b, u32 c, u32 d)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetTevColorOp` | `void GXSetTevColorOp(u32 stage, u32 op, u32 bias, u32 scale, GXBool clamp, u32 out_reg)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetTevAlphaOp` | `void GXSetTevAlphaOp(u32 stage, u32 op, u32 bias, u32 scale, GXBool clamp, u32 out_reg)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetTevOrder` | `void GXSetTevOrder(u32 stage, u32 coord, u32 map, u32 color)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetTevColor` | `void GXSetTevColor(u32 id, u32 color_packed)` | pc_gx.c | rewrite-for-KOS | TEV register colors: PVR has no TEV registers; these collapse into vertex colors or a second pass. |
| `GXSetTevColorS10` | `void GXSetTevColorS10(u32 id, s16 r, s16 g, s16 b, s16 a)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetTevKColor` | `void GXSetTevKColor(u32 id, u32 color_packed)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetTevKColorSel` | `void GXSetTevKColorSel(u32 stage, u32 sel)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetTevKAlphaSel` | `void GXSetTevKAlphaSel(u32 stage, u32 sel)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetTevSwapMode` | `void GXSetTevSwapMode(u32 stage, u32 ras_sel, u32 tex_sel)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetTevSwapModeTable` | `void GXSetTevSwapModeTable(u32 table, u32 red, u32 green, u32 blue, u32 alpha)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetTevDirect` | `void GXSetTevDirect(u32 stage)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetNumIndStages` | `void GXSetNumIndStages(u8 n)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetIndTexMtx` | `void GXSetIndTexMtx(u32 mtx_sel, const void* offset, s8 scale)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetIndTexOrder` | `void GXSetIndTexOrder(u32 ind_stage, u32 tex_coord, u32 tex_map)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetTevIndirect` | `void GXSetTevIndirect(u32 stage, u32 ind_stage, u32 fmt, u32 bias_sel, u32 mtx_sel, u32 wrap_s, u32 wrap_t, GXBool add_prev, GXBool ind_lod, u32 alpha_sel)` | pc_gx.c | rewrite-for-KOS | Indirect-texture stages. PVR has no equivalent — the 101-config census must say whether AC ever uses them. |
| `GXSetTevIndWarp` | `void GXSetTevIndWarp(u32 stage, u32 ind_stage, GXBool signed_ofs, GXBool replace, u32 mtx_sel)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetIndTexCoordScale` | `void GXSetIndTexCoordScale(u32 ind_stage, u32 scale_s, u32 scale_t)` | pc_gx.c | rewrite-for-KOS |  |
| `__GXSetIndirectMask` | `void __GXSetIndirectMask(u32 mask)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetZTexture` | `void GXSetZTexture(u32 op, u32 fmt, u32 bias)` | pc_gx.c | rewrite-for-KOS |  |

### GX — raster / blend / depth / fog

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `GXSetAlphaCompare` | `void GXSetAlphaCompare(u32 comp0, u8 ref0, u32 op, u32 comp1, u8 ref1)` | pc_gx.c | rewrite-for-KOS | ★ Drives the OPAQUE / PUNCH-THROUGH / TRANSLUCENT list classification on PVR — the most consequential single mapping decision in the renderer. |
| `GXSetBlendMode` | `void GXSetBlendMode(u32 type, u32 src, u32 dst, u32 logic_op)` | pc_gx.c | rewrite-for-KOS | Maps to PVR per-poly blend modes; PVR sorts translucents per-tile so draw order matters much less. |
| `GXSetZMode` | `void GXSetZMode(GXBool compare_enable, u32 func, GXBool update_enable)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetColorUpdate` | `void GXSetColorUpdate(GXBool enable)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetAlphaUpdate` | `void GXSetAlphaUpdate(GXBool enable)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetZCompLoc` | `void GXSetZCompLoc(GXBool before_tex)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetDither` | `void GXSetDither(GXBool dither)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetDstAlpha` | `void GXSetDstAlpha(GXBool enable, u8 alpha)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetFieldMask` | `void GXSetFieldMask(GXBool odd, GXBool even)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetFieldMode` | `void GXSetFieldMode(GXBool field_mode, GXBool half_aspect)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetPixelFmt` | `void GXSetPixelFmt(u32 pix_fmt, u32 z_fmt)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetCullMode` | `void GXSetCullMode(u32 mode)` | pc_gx.c | rewrite-for-KOS | GX cull sense is opposite to GL's in places; verify against the PC implementation before trusting it. |
| `GXSetCoPlanar` | `void GXSetCoPlanar(GXBool enable)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetFog` | `void GXSetFog(u32 type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color)` | pc_gx.c | rewrite-for-KOS | PVR table fog is a near-native fit for GX fog. |
| `GXInitFogAdjTable` | `void GXInitFogAdjTable(void* table, u16 width, f32 projmtx[4][4])` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetFogRangeAdj` | `void GXSetFogRangeAdj(GXBool enable, u16 center, void* table)` | pc_gx.c | rewrite-for-KOS |  |

### GX — lighting & color channels

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `GXSetNumChans` | `void GXSetNumChans(u8 nChans)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetChanCtrl` | `void GXSetChanCtrl(u32 chan, GXBool enable, u32 amb_src, u32 mat_src, u32 light_mask, u32 diff_fn, u32 attn_fn)` | pc_gx.c | rewrite-for-KOS | 8 lights, ambient/material source select, diffuse fn + attenuation fn. Runs in the vertex shader on PC; on DC this is SH-4 work (FIPR). |
| `GXSetChanAmbColor` | `void GXSetChanAmbColor(u32 chan, u32 color_packed)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetChanMatColor` | `void GXSetChanMatColor(u32 chan, u32 color_packed)` | pc_gx.c | rewrite-for-KOS |  |
| `GXInitLightSpot` | `void GXInitLightSpot(void* lt, f32 cutoff, u32 spot_func)` | pc_gx.c | rewrite-for-KOS |  |
| `GXInitLightDistAttn` | `void GXInitLightDistAttn(void* lt, f32 ref_dist, f32 ref_bright, u32 dist_func)` | pc_gx.c | rewrite-for-KOS |  |
| `GXInitLightPos` | `void GXInitLightPos(void* lt, f32 x, f32 y, f32 z)` | pc_gx.c | rewrite-for-KOS |  |
| `GXInitLightDir` | `void GXInitLightDir(void* lt, f32 nx, f32 ny, f32 nz)` | pc_gx.c | rewrite-for-KOS |  |
| `GXInitLightColor` | `void GXInitLightColor(void* lt, u32 color)` | pc_gx.c | rewrite-for-KOS |  |
| `GXInitLightAttn` | `void GXInitLightAttn(void* lt, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2)` | pc_gx.c | rewrite-for-KOS | Angular (a0,a1,a2) + distance (k0,k1,k2) attenuation. Full GC model; clamp to measured usage on DC. |
| `GXInitLightAttnA` | `void GXInitLightAttnA(void* lt, f32 a0, f32 a1, f32 a2)` | pc_gx.c | rewrite-for-KOS |  |
| `GXInitLightAttnK` | `void GXInitLightAttnK(void* lt, f32 k0, f32 k1, f32 k2)` | pc_gx.c | rewrite-for-KOS |  |
| `GXLoadLightObjImm` | `void GXLoadLightObjImm(void* lt, u32 light)` | pc_gx.c | rewrite-for-KOS |  |
| `GXGetLightPos` | `void GXGetLightPos(void* lt, f32* x, f32* y, f32* z)` | pc_gx.c | rewrite-for-KOS |  |
| `GXGetLightColor` | `void GXGetLightColor(void* lt, void* color)` | pc_gx.c | rewrite-for-KOS |  |

### GX — texgen

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `GXSetNumTexGens` | `void GXSetNumTexGens(u8 n)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetTexCoordGen2` | `void GXSetTexCoordGen2(u32 dst, u32 func, u32 src, u32 mtx, GXBool normalize, u32 postmtx)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetLineWidth` | `void GXSetLineWidth(u8 width, u32 texOffsets)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetPointSize` | `void GXSetPointSize(u8 size, u32 texOffsets)` | pc_gx.c | rewrite-for-KOS |  |
| `GXEnableTexOffsets` | `void GXEnableTexOffsets(u32 coord, GXBool line, GXBool point)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetTexCoordScaleManually` | `void GXSetTexCoordScaleManually(u32 coord, GXBool enable, u16 ss, u16 ts)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetTexCoordBias` | `void GXSetTexCoordBias(u32 coord, u8 s, u8 t)` | pc_gx.c | rewrite-for-KOS |  |

### GX — display copy

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `GXSetCopyClear` | `void GXSetCopyClear(GXColor clear_clr, u32 clear_z)` | pc_gx.c | rewrite-for-KOS | Clear color/Z for the next copy; on DC feeds pvr_set_bg_color / the OP list background plane. |
| `GXCopyDisp` | `void GXCopyDisp(void* dest, GXBool clear)` | pc_gx.c | rewrite-for-KOS | PC only flushes geometry; the swap happens in VIWaitForRetrace. On DC this is where pvr_scene_finish() belongs (or stays a flush, with the swap in VI). |
| `GXSetDispCopyGamma` | `void GXSetDispCopyGamma(u32 gamma)` | pc_gx.c | port-as-is |  |
| `GXSetDispCopySrc` | `void GXSetDispCopySrc(u16 left, u16 top, u16 wd, u16 ht)` | pc_gx.c | port-as-is |  |
| `GXSetDispCopyDst` | `void GXSetDispCopyDst(u16 wd, u16 ht)` | pc_gx.c | port-as-is |  |
| `GXGetYScaleFactor` | `f32 GXGetYScaleFactor(u16 efbHeight, u16 xfbHeight)` | pc_gx.c | port-as-is |  |
| `GXSetDispCopyYScale` | `u32 GXSetDispCopyYScale(f32 vscale)` | pc_gx.c | port-as-is |  |
| `GXGetNumXfbLines` | `u16 GXGetNumXfbLines(u16 efbHeight, f32 yScale)` | pc_gx.c | port-as-is |  |
| `GXSetCopyFilter` | `void GXSetCopyFilter(GXBool aa, const void* pattern, GXBool vf, const void* vfilter)` | pc_gx.c | port-as-is |  |
| `GXAdjustForOverscan` | `void GXAdjustForOverscan(void* rmin, void* rmout, u16 hor, u16 ver)` | pc_gx.c | port-as-is |  |

### GX — EFB copy / render-to-texture

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `pc_gx_efb_capture_store` ✱ | `void pc_gx_efb_capture_store(u32 dest_ptr, GLuint gl_tex)` | pc_gx.c | rewrite-for-KOS |  |
| `pc_gx_efb_capture_find` ✱ | `GLuint pc_gx_efb_capture_find(u32 data_ptr)` | pc_gx.c | rewrite-for-KOS |  |
| `pc_gx_efb_capture_cleanup` ✱ | `void pc_gx_efb_capture_cleanup(void)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetTexCopySrc` | `void GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht)` | pc_gx.c | rewrite-for-KOS |  |
| `GXSetTexCopyDst` | `void GXSetTexCopyDst(u16 wd, u16 ht, u32 fmt, GXBool mipmap)` | pc_gx.c | rewrite-for-KOS |  |
| `GXCopyTex` | `void GXCopyTex(void* dest, GXBool clear)` | pc_gx.c | rewrite-for-KOS | ★ EFB->texture capture. PC does an FBO blit. PVR render-to-texture is costly; enumerate the AC call sites (PLAN §3.3) and handle per-case. |
| `GXSetCopyClamp` | `void GXSetCopyClamp(u32 clamp)` | pc_gx.c | rewrite-for-KOS |  |

### GX — display lists

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `GXBeginDisplayList` | `void GXBeginDisplayList(void* list, u32 size)` | pc_gx.c | rewrite-for-KOS |  |
| `GXEndDisplayList` | `u32 GXEndDisplayList(void)` | pc_gx.c | rewrite-for-KOS |  |
| `GXCallDisplayList` | `void GXCallDisplayList(void* list, u32 nbytes)` | pc_gx.c | rewrite-for-KOS |  |

### GX — FIFO / write-gather (all no-ops)

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `GXInitFifoBase` | `void GXInitFifoBase(GXFifoObj* fifo, void* base, u32 size)` | pc_gx.c | stub-and-log |  |
| `GXInitFifoPtrs` | `void GXInitFifoPtrs(GXFifoObj* fifo, void* rp, void* wp)` | pc_gx.c | stub-and-log |  |
| `GXInitFifoLimits` | `void GXInitFifoLimits(GXFifoObj* fifo, u32 hi, u32 lo)` | pc_gx.c | stub-and-log |  |
| `GXSetCPUFifo` | `void GXSetCPUFifo(GXFifoObj* fifo)` | pc_gx.c | stub-and-log |  |
| `GXSetGPFifo` | `void GXSetGPFifo(GXFifoObj* fifo)` | pc_gx.c | stub-and-log |  |
| `GXSaveCPUFifo` | `void GXSaveCPUFifo(GXFifoObj* fifo)` | pc_gx.c | stub-and-log |  |
| `GXSaveGPFifo` | `void GXSaveGPFifo(GXFifoObj* fifo)` | pc_gx.c | stub-and-log |  |
| `GXGetGPStatus` | `void GXGetGPStatus(GXBool* a, GXBool* b, GXBool* c, GXBool* d, GXBool* e)` | pc_gx.c | stub-and-log |  |
| `GXGetFifoStatus` | `void GXGetFifoStatus(GXFifoObj* f, GXBool* a, GXBool* b, u32* c, GXBool* d, GXBool* e, GXBool* g)` | pc_gx.c | stub-and-log |  |
| `GXGetFifoPtrs` | `void GXGetFifoPtrs(GXFifoObj* f, void** rp, void** wp)` | pc_gx.c | stub-and-log |  |
| `GXGetFifoBase` | `void* GXGetFifoBase(GXFifoObj* f)` | pc_gx.c | stub-and-log |  |
| `GXGetFifoSize` | `u32 GXGetFifoSize(GXFifoObj* f)` | pc_gx.c | stub-and-log |  |
| `GXGetFifoLimits` | `void GXGetFifoLimits(GXFifoObj* f, u32* hi, u32* lo)` | pc_gx.c | stub-and-log |  |
| `GXSetBreakPtCallback` | `void* GXSetBreakPtCallback(void* cb)` | pc_gx.c | stub-and-log |  |
| `GXEnableBreakPt` | `void GXEnableBreakPt(void* bp)` | pc_gx.c | stub-and-log |  |
| `GXDisableBreakPt` | `void GXDisableBreakPt(void)` | pc_gx.c | stub-and-log |  |
| `GXSetCurrentGXThread` | `void* GXSetCurrentGXThread(void)` | pc_gx.c | stub-and-log |  |
| `GXGetCurrentGXThread` | `void* GXGetCurrentGXThread(void)` | pc_gx.c | stub-and-log |  |
| `GXGetCPUFifo` | `GXFifoObj* GXGetCPUFifo(void)` | pc_gx.c | stub-and-log |  |
| `GXGetGPFifo` | `GXFifoObj* GXGetGPFifo(void)` | pc_gx.c | stub-and-log |  |
| `GXGetOverflowCount` | `u32 GXGetOverflowCount(void)` | pc_gx.c | stub-and-log |  |
| `GXResetOverflowCount` | `u32 GXResetOverflowCount(void)` | pc_gx.c | stub-and-log |  |
| `GXRedirectWriteGatherPipe` | `volatile void* GXRedirectWriteGatherPipe(void* ptr)` | pc_gx.c | stub-and-log |  |
| `GXRestoreWriteGatherPipe` | `void GXRestoreWriteGatherPipe(void)` | pc_gx.c | stub-and-log |  |
| `IsWriteGatherBufferEmpty` | `int IsWriteGatherBufferEmpty(void)` | pc_gx.c | stub-and-log | GC write-gather pipe; always empty. All FIFO functions are pure no-ops with 0 semantic content. |

### GX — unused / verify

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `GXDrawSphere` | `void GXDrawSphere(u8 numMajor, u8 numMinor)` | pc_gx.c | drop |  |
| `GXReadXfRasMetric` | `void GXReadXfRasMetric(u32* xf_wait_in, u32* xf_wait_out, u32* ras_busy, u32* clocks)` | pc_gx.c | drop |  |
| `GXSetVerifyLevel` | `void GXSetVerifyLevel(u32 level)` | pc_gx.c | drop |  |
| `GXSetVerifyCallback` | `void* GXSetVerifyCallback(void* cb)` | pc_gx.c | drop |  |

### GX — platform-internal helpers

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `pc_gx_flush_if_begin_complete` ✱ | `void pc_gx_flush_if_begin_complete(void)` | pc_gx.c | rewrite-for-KOS |  |
| `pc_gx_mark_dirty` ✱ | `void pc_gx_mark_dirty(unsigned int flag)` | pc_gx.c | rewrite-for-KOS |  |
| `pc_gx_init` ✱ | `void pc_gx_init(void)` | pc_gx.c | rewrite-for-KOS |  |
| `pc_gx_begin_frame` | `void pc_gx_begin_frame(void)` | pc_gx.c | rewrite-for-KOS |  |
| `pc_gx_restore_after_nes` ✱ | `void pc_gx_restore_after_nes(void)` | pc_gx.c | rewrite-for-KOS |  |
| `pc_gx_shutdown` ✱ | `void pc_gx_shutdown(void)` | pc_gx.c | rewrite-for-KOS |  |
| `pc_gx_blit_to_screen` ✱ | `void pc_gx_blit_to_screen(void)` | pc_gx.c | rewrite-for-KOS |  |
| `pc_gx_fill_uniform_locations` ✱ | `void pc_gx_fill_uniform_locations(GLuint shader, PCGXUniformLocs* u)` | pc_gx.c | rewrite-for-KOS |  |
| `pc_gx_flush_vertices` ✱ | `void pc_gx_flush_vertices(void)` | pc_gx.c | rewrite-for-KOS |  |
| `pc_gx_frame_timing_snapshot` ✱ | `void pc_gx_frame_timing_snapshot(void)` | pc_gx.c | rewrite-for-KOS |  |

### GX — texture objects, TLUT, texture cache

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `pc_gx_texture_cache_invalidate` ✱ | `void pc_gx_texture_cache_invalidate(void)` | pc_gx_texture.c | rewrite-for-KOS |  |
| `pc_gx_texture_init` ✱ | `void pc_gx_texture_init(void)` | pc_gx_texture.c | rewrite-for-KOS |  |
| `pc_gx_texture_shutdown` ✱ | `void pc_gx_texture_shutdown(void)` | pc_gx_texture.c | rewrite-for-KOS |  |
| `GXInitTexObj` | `void GXInitTexObj(void* obj, void* image_ptr, u16 width, u16 height, u32 format, u32 wrap_s, u32 wrap_t, u8 mipmap)` | pc_gx_texture.c | rewrite-for-KOS | ★ TexObj is an opaque blob the GAME allocates (GXTexObj, 32 bytes) — the DC layer must fit its state in the same footprint or the game's structs overflow. |
| `GXInitTexObjCI` | `void GXInitTexObjCI(void* obj, void* image_ptr, u16 width, u16 height, u32 format, u32 wrap_s, u32 wrap_t, u8 mipmap, u32 tlut_name)` | pc_gx_texture.c | rewrite-for-KOS |  |
| `GXInitTexObjData` | `void GXInitTexObjData(void* obj, void* image_ptr)` | pc_gx_texture.c | rewrite-for-KOS |  |
| `GXInitTexObjLOD` | `void GXInitTexObjLOD(void* obj, u32 min_filt, u32 mag_filt, f32 min_lod, f32 max_lod, f32 lod_bias, GXBool bias_clamp, GXBool edge_lod, u32 max_aniso)` | pc_gx_texture.c | rewrite-for-KOS |  |
| `GXInitTexObjWrapMode` | `void GXInitTexObjWrapMode(void* obj, u32 s, u32 t)` | pc_gx_texture.c | rewrite-for-KOS |  |
| `GXLoadTexObj` | `void GXLoadTexObj(void* obj, u32 id)` | pc_gx_texture.c | rewrite-for-KOS | Binds to a texture unit id (GX_TEXMAP0..7). PVR has ONE texture unit — multi-map TEV stages become extra passes. |
| `GXGetTexBufferSize` | `u32 GXGetTexBufferSize(u16 width, u16 height, u32 format, GXBool mipmap, u8 max_lod)` | pc_gx_texture.c | rewrite-for-KOS | Pure arithmetic over GC texture formats; used by the game to size allocations. Must keep returning GC sizes even though DC stores twiddled/VQ data elsewhere. |
| `GXInvalidateTexAll` | `void GXInvalidateTexAll(void)` | pc_gx_texture.c | rewrite-for-KOS |  |
| `GXInvalidateTexRegion` | `void GXInvalidateTexRegion(void* region)` | pc_gx_texture.c | rewrite-for-KOS |  |
| `GXInitTlutObj` | `void GXInitTlutObj(void* obj, void* lut, u32 fmt, u16 n_entries)` | pc_gx_texture.c | rewrite-for-KOS | TLUT (palette) object. CI4/CI8 map to PVR native 4/8-bit paletted textures — a VRAM win. |
| `GXLoadTlut` | `void GXLoadTlut(void* obj, u32 idx)` | pc_gx_texture.c | rewrite-for-KOS |  |
| `pc_gx_tlut_set_native_le` | `void pc_gx_tlut_set_native_le(unsigned int idx)` | pc_gx_texture.c | rewrite-for-KOS | ★ Per-slot is_be flag: ROM-sourced TLUTs are big-endian, emu64/EFB ones are native LE. This distinction survives on DC unchanged. |
| `GXInitTexCacheRegion` | `void GXInitTexCacheRegion(void* region, GXBool is_32b, u32 tmem_even, u32 size_even, u32 tmem_odd, u32 size_odd)` | pc_gx_texture.c | rewrite-for-KOS |  |
| `GXSetTexRegionCallback` | `void* GXSetTexRegionCallback(void* callback)` | pc_gx_texture.c | rewrite-for-KOS |  |
| `GXInitTlutRegion` | `void GXInitTlutRegion(void* region, u32 tmem_addr, u32 tlut_size)` | pc_gx_texture.c | rewrite-for-KOS |  |
| `GXGetTexObjMipMap` | `GXBool GXGetTexObjMipMap(const void* obj)` | pc_gx_texture.c | rewrite-for-KOS |  |
| `GXGetTexObjFmt` | `u32 GXGetTexObjFmt(const void* obj)` | pc_gx_texture.c | rewrite-for-KOS |  |
| `GXGetTexObjHeight` | `u16 GXGetTexObjHeight(const void* obj)` | pc_gx_texture.c | rewrite-for-KOS |  |
| `GXGetTexObjWidth` | `u16 GXGetTexObjWidth(const void* obj)` | pc_gx_texture.c | rewrite-for-KOS |  |
| `GXGetTexObjWrapS` | `u32 GXGetTexObjWrapS(const void* obj)` | pc_gx_texture.c | rewrite-for-KOS |  |
| `GXGetTexObjWrapT` | `u32 GXGetTexObjWrapT(const void* obj)` | pc_gx_texture.c | rewrite-for-KOS |  |
| `GXGetTexObjData` | `void* GXGetTexObjData(const void* obj)` | pc_gx_texture.c | rewrite-for-KOS |  |
| `GXDestroyTexObj` | `void GXDestroyTexObj(void* obj)` | pc_gx_texture.c | rewrite-for-KOS |  |
| `GXDestroyTlutObj` | `void GXDestroyTlutObj(void* obj)` | pc_gx_texture.c | rewrite-for-KOS |  |

### GX — TEV→GLSL shader generator

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `pc_gx_tev_init` ✱ | `void pc_gx_tev_init(void)` | pc_gx_tev.c | drop |  |
| `pc_gx_tev_shutdown` ✱ | `void pc_gx_tev_shutdown(void)` | pc_gx_tev.c | drop |  |
| `pc_gx_tev_get_shader` ✱ | `GLuint pc_gx_tev_get_shader(PCGXState* state)` | pc_gx_tev.c | drop |  |

### VI — video interface & frame pacing

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `VIConfigurePan` | `void VIConfigurePan(u16 x_origin, u16 y_origin, u16 width, u16 height)` | pc_stubs.c | port-as-is |  |
| `VIInit` | `void VIInit(void)` | pc_vi.c | rewrite-for-KOS |  |
| `VIConfigure` | `void VIConfigure(void* rm)` | pc_vi.c | rewrite-for-KOS | Takes a GXRenderModeObj (the GXNtsc480IntDf[] blobs in pc_misc.c are 64 zero bytes). On DC this must at minimum select 640x480 NTSC vs PAL. |
| `VISetNextFrameBuffer` | `void VISetNextFrameBuffer(void* fb)` | pc_vi.c | rewrite-for-KOS | No-op on PC (renders straight to the back buffer). |
| `VIFlush` | `void VIFlush(void)` | pc_vi.c | rewrite-for-KOS |  |
| `pc_dynamic_fps_reset` ✱ | `void pc_dynamic_fps_reset(void)` | pc_vi.c | rewrite-for-KOS |  |
| `VIWaitForRetrace` | `void VIWaitForRetrace(void)` | pc_vi.c | rewrite-for-KOS | ★ The frame heartbeat: pumps events, does the swap, runs the frame-pacing wait, computes the dynamic-FPS target, and calls pc_gx_begin_frame(). Everything about pacing lives here. On DC it becomes pvr_scene_finish()/vid_waitvbl plus the same pacing logic; PLAN targets 30 fps. |
| `VIGetRetraceCount` | `u32 VIGetRetraceCount(void)` | pc_vi.c | rewrite-for-KOS | Monotonic frame counter the game uses for timing. |
| `VISetBlack` | `void VISetBlack(BOOL black)` | pc_vi.c | rewrite-for-KOS |  |
| `VIGetTvFormat` | `u32 VIGetTvFormat(void)` | pc_vi.c | rewrite-for-KOS |  |
| `VIGetDTVStatus` | `u32 VIGetDTVStatus(void)` | pc_vi.c | rewrite-for-KOS |  |
| `VISetPreRetraceCallback` | `void* VISetPreRetraceCallback(void* cb)` | pc_vi.c | rewrite-for-KOS |  |
| `VISetPostRetraceCallback` | `void* VISetPostRetraceCallback(void* cb)` | pc_vi.c | rewrite-for-KOS |  |
| `VIGetCurrentLine` | `u32 VIGetCurrentLine(void)` | pc_vi.c | rewrite-for-KOS |  |
| `VISetNextXFB` ✱ | `void VISetNextXFB(void* xfb)` | pc_vi.c | rewrite-for-KOS |  |

### PAD — controller

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `pc_pad_dpad_as_stick_active` ✱ | `int pc_pad_dpad_as_stick_active(void)` | pc_pad.c | rewrite-for-KOS |  |
| `PADInit` | `BOOL PADInit(void)` | pc_pad.c | rewrite-for-KOS |  |
| `PADRead` | `u32 PADRead(PADStatus* status)` | pc_pad.c | rewrite-for-KOS | Fills PADStatus[4]; then JUTGamePad calls PADClamp() (compiled from src/static/dolphin/pad/Padclamp.c — the ONE Dolphin SDK source the PC build keeps). DC maple pad has no C-stick/Z; see PLAN §7. |
| `PADControlMotor` | `void PADControlMotor(s32 chan, u32 command)` | pc_pad.c | rewrite-for-KOS |  |
| `PADControlAllMotors` | `void PADControlAllMotors(const u32* commands)` | pc_pad.c | rewrite-for-KOS |  |
| `PADCleanup` ✱ | `void PADCleanup(void)` | pc_pad.c | rewrite-for-KOS |  |
| `PADReset` | `BOOL PADReset(u32 mask)` | pc_pad.c | rewrite-for-KOS |  |
| `PADRecalibrate` | `BOOL PADRecalibrate(u32 mask)` | pc_pad.c | rewrite-for-KOS |  |
| `PADSync` | `BOOL PADSync(void)` | pc_pad.c | rewrite-for-KOS |  |
| `PADSetSpec` | `void PADSetSpec(u32 spec)` | pc_pad.c | rewrite-for-KOS |  |
| `PADSetAnalogMode` | `void PADSetAnalogMode(u32 mode)` | pc_pad.c | rewrite-for-KOS |  |
| `PADGetType` | `BOOL PADGetType(s32 chan, u32* type)` | pc_pad.c | rewrite-for-KOS |  |
| `pc_pad_get_controller` ✱ | `SDL_GameController* pc_pad_get_controller(void)` | pc_pad.c | rewrite-for-KOS |  |

### DVD — file I/O (async faked as sync)

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `DVDGetCurrentDiskID` | `DVDDiskID* DVDGetCurrentDiskID(void)` | pc_dvd.c | rewrite-for-KOS |  |
| `DVDConvertPathToEntrynum` | `s32 DVDConvertPathToEntrynum(const char* path)` | pc_dvd.c | rewrite-for-KOS | ★ PC fakes the FST: it interns any path into a growing table (MAX_DVD_ENTRIES=512) and returns the index, so it NEVER fails for an unknown path. Real GC returns -1. Keep the interning on DC or code that checks for -1 changes behavior. |
| `DVDFastOpen` | `BOOL DVDFastOpen(s32 entrynum, void* fileInfo)` | pc_dvd.c | rewrite-for-KOS | ★ Writes into the game's DVDFileInfo at hand-picked offsets: FILE*/sentinel at +0x18, startAddr at +0x30, length at +0x34, total 0x3C bytes. Layout-critical. |
| `DVDOpen` | `BOOL DVDOpen(const char* filename, void* fileInfo)` | pc_dvd.c | rewrite-for-KOS |  |
| `DVDClose` | `BOOL DVDClose(void* fileInfo)` | pc_dvd.c | rewrite-for-KOS |  |
| `DVDReadPrio` | `s32 DVDReadPrio(void* fileInfo, void* buf, s32 length, s32 offset, s32 prio)` | pc_dvd.c | rewrite-for-KOS | Synchronous fread/disc-image read. Blocking. CD-R at ~500 KB/s makes this the read-ahead seam (PLAN §5). |
| `DVDRead` | `s32 DVDRead(void* fileInfo, void* buf, s32 length, s32 offset)` | pc_dvd.c | rewrite-for-KOS |  |
| `DVDGetLength` ✱ | `u32 DVDGetLength(void* fileInfo)` | pc_dvd.c | rewrite-for-KOS |  |
| `DVDReadAsyncPrio` | `BOOL DVDReadAsyncPrio(void* fileInfo, void* buf, s32 length, s32 offset, pc_DVDCallback callback, s32 prio)` | pc_dvd.c | rewrite-for-KOS | ★ ASYNC FAKED AS SYNC: performs the whole read inline and calls the callback BEFORE returning TRUE. Any game code that assumed 'callback fires later' now sees it fire first — this is load-bearing and already validated on the base port. A real read-ahead thread on DC must preserve callback-before-return or re-validate every caller. |
| `OSDVDFatalError` | `void OSDVDFatalError(void)` | pc_dvd.c | rewrite-for-KOS |  |
| `DVDInit` | `void DVDInit(void)` | pc_dvd.c | rewrite-for-KOS |  |
| `DVDSetAutoFatalMessaging` ✱ | `void DVDSetAutoFatalMessaging(BOOL enable)` | pc_dvd.c | rewrite-for-KOS |  |
| `DVDGetFileInfoStatus` | `s32 DVDGetFileInfoStatus(void* fileInfo)` | pc_dvd.c | rewrite-for-KOS |  |
| `DVDGetTransferredSize` | `s32 DVDGetTransferredSize(void* fileInfo)` | pc_dvd.c | rewrite-for-KOS |  |
| `DVDFastClose` ✱ | `BOOL DVDFastClose(void* fileInfo)` | pc_dvd.c | rewrite-for-KOS |  |
| `DVDGetDriveStatus` | `s32 DVDGetDriveStatus(void)` | pc_dvd.c | rewrite-for-KOS |  |
| `DVDCancel` | `s32 DVDCancel(void* block)` | pc_dvd.c | rewrite-for-KOS |  |
| `DVDCancelAsync` | `BOOL DVDCancelAsync(void* block, void* callback)` | pc_dvd.c | rewrite-for-KOS |  |
| `DVDChangeDisk` | `s32 DVDChangeDisk(void* block, void* id)` | pc_dvd.c | rewrite-for-KOS |  |
| `DVDChangeDiskAsync` | `BOOL DVDChangeDiskAsync(void* block, void* id, void* callback)` | pc_dvd.c | rewrite-for-KOS |  |
| `DVDGetCommandBlockStatus` | `s32 DVDGetCommandBlockStatus(void* block)` | pc_dvd.c | rewrite-for-KOS | Always 0 (DVD_STATE_END) because everything already completed. |
| `DVDPrepareStreamAsync` | `BOOL DVDPrepareStreamAsync(void* fi, u32 len, u32 off, void* cb)` | pc_dvd.c | rewrite-for-KOS |  |
| `DVDCancelStream` | `s32 DVDCancelStream(void* block)` | pc_dvd.c | rewrite-for-KOS |  |

### DVD — directory stubs

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `DVDCheckDisk` | `BOOL DVDCheckDisk(void)` | pc_stubs.c | stub-and-log |  |
| `DVDOpenDir` | `BOOL DVDOpenDir(char* dirName, void* dir)` | pc_stubs.c | stub-and-log | Directory enumeration — pure stub, 0 real callers. |
| `DVDReadDir` | `BOOL DVDReadDir(void* dir, void* dirEntry)` | pc_stubs.c | stub-and-log |  |
| `DVDCloseDir` | `BOOL DVDCloseDir(void* dir)` | pc_stubs.c | stub-and-log |  |

### AR / ARQ — auxiliary RAM DMA

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `ARInit` | `u32 ARInit(u32* stack_idx_addr, u32 length)` | pc_aram.c | rewrite-for-KOS | ★ Returns 0: ARAM addresses are OFFSETS from 0, not pointers. jsyswrap requests soundAram 0x810000 + graphAram 0x6A3780 = 16 MB. On DC this whole 16 MB cannot exist; PLAN §3.1 turns the graph half into a disc-backed LRU window and the sound half into AICA/stream data. |
| `pc_aram_get_base` ✱ | `u8* pc_aram_get_base(void)` | pc_aram.c | rewrite-for-KOS |  |
| `ARGetBaseAddress` | `u32 ARGetBaseAddress(void)` | pc_aram.c | rewrite-for-KOS |  |
| `ARGetSize` | `u32 ARGetSize(void)` | pc_aram.c | rewrite-for-KOS |  |
| `ARAlloc` | `u32 ARAlloc(u32 size)` | pc_aram.c | rewrite-for-KOS | Bump allocator, 32-byte aligned, never frees (ARFree is a no-op). |
| `ARFree` | `void ARFree(u32* addr)` | pc_aram.c | rewrite-for-KOS |  |
| `ARStartDMA` | `void ARStartDMA(u32 type, u32 mram_addr, u32 aram_addr, u32 length)` | pc_aram.c | rewrite-for-KOS | ★ THE ARAM SEAM. type 0 = MRAM->ARAM, 1 = ARAM->MRAM; it is a memcpy today. It also DEFENSIVELY normalizes aram_addr when a caller passes `aram_base + offset` instead of a bare offset, and zero-fills the destination on an out-of-range ARAM->MRAM read (cap 1 MB) so callers get zeros, not garbage. Both behaviors are relied on. This function is where disc-backed ARAM must be implemented. |
| `ARGetInternalSize` | `u32 ARGetInternalSize(void)` | pc_aram.c | rewrite-for-KOS |  |
| `ARCheckInit` | `BOOL ARCheckInit(void)` | pc_aram.c | rewrite-for-KOS |  |
| `ARQInit` | `void ARQInit(void)` | pc_aram.c | rewrite-for-KOS |  |
| `ARQPostRequest` | `void ARQPostRequest(void* req, u32 owner, u32 type, u32 prio, u32 source, u32 dest, u32 length, void* callback)` | pc_aram.c | rewrite-for-KOS | ★ Argument-order trap: ARQ's (source,dest) is NOT ARStartDMA's (mram,aram) — for type 1 the two are swapped. Completion callback is invoked synchronously with the request pointer as a u32. |
| `ARQFlushQueue` ✱ | `void ARQFlushQueue(void)` | pc_aram.c | rewrite-for-KOS | No-op because everything is synchronous. 0 call sites. |

### AI — audio interface

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `AIInit` | `void AIInit(u8* stack)` | pc_audio.c | rewrite-for-KOS | Opens the output device. On DC: KOS snd_stream. |
| `AIInitDMA` | `void AIInitDMA(u32 addr, u32 size)` | pc_audio.c | rewrite-for-KOS | ★ THE AUDIO HANDOFF POINT. jaudio calls it with (addr,size) of a finished 32 kHz s16 stereo block; the PC layer copies into a 32768-sample SPSC ring drained by the audio callback. Everything upstream (rspsim software DSP) is decompiled game code that keeps working. |
| `AIStartDMA` | `void AIStartDMA(void)` | pc_audio.c | rewrite-for-KOS |  |
| `AIStopDMA` | `void AIStopDMA(void)` | pc_audio.c | rewrite-for-KOS |  |
| `AIGetDMAStartAddr` | `u32 AIGetDMAStartAddr(void)` | pc_audio.c | rewrite-for-KOS |  |
| `AIGetDMALength` | `u16 AIGetDMALength(void)` | pc_audio.c | rewrite-for-KOS |  |
| `AIGetStreamTrigger` | `u32 AIGetStreamTrigger(void)` | pc_audio.c | rewrite-for-KOS |  |
| `AIGetStreamSampleCount` | `u32 AIGetStreamSampleCount(void)` | pc_audio.c | rewrite-for-KOS |  |
| `AISetStreamPlayState` | `void AISetStreamPlayState(u32 state)` | pc_audio.c | rewrite-for-KOS |  |
| `AIGetStreamPlayState` | `u32 AIGetStreamPlayState(void)` | pc_audio.c | rewrite-for-KOS |  |
| `AISetStreamSampleRate` | `void AISetStreamSampleRate(u32 rate)` | pc_audio.c | rewrite-for-KOS |  |
| `AIGetStreamSampleRate` | `u32 AIGetStreamSampleRate(void)` | pc_audio.c | rewrite-for-KOS |  |
| `AISetStreamVolLeft` | `void AISetStreamVolLeft(u8 vol)` | pc_audio.c | rewrite-for-KOS |  |
| `AISetStreamVolRight` | `void AISetStreamVolRight(u8 vol)` | pc_audio.c | rewrite-for-KOS |  |
| `AIGetStreamVolLeft` | `u8 AIGetStreamVolLeft(void)` | pc_audio.c | rewrite-for-KOS |  |
| `AIGetStreamVolRight` | `u8 AIGetStreamVolRight(void)` | pc_audio.c | rewrite-for-KOS |  |
| `AIResetStreamSampleCount` | `void AIResetStreamSampleCount(void)` | pc_audio.c | rewrite-for-KOS |  |
| `AISetDSPSampleRate` | `void AISetDSPSampleRate(u32 rate)` | pc_audio.c | rewrite-for-KOS | Game runs at 32 kHz; PLAN §3.4 stage A drops to 22.05 kHz on DC. |
| `AIGetDSPSampleRate` | `u32 AIGetDSPSampleRate(void)` | pc_audio.c | rewrite-for-KOS |  |
| `AIRegisterDMACallback` | `void* AIRegisterDMACallback(void* callback)` | pc_audio.c | rewrite-for-KOS |  |

### DSP — mailbox (rspsim does the work)

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `DSPInit` | `void DSPInit(void)` | pc_audio.c | port-as-is | All DSP mailbox functions are stubs — rspsim (src/static/jaudio_NES) does the synthesis in software. Compile unchanged. |
| `DSPCheckMailToDSP` | `BOOL DSPCheckMailToDSP(void)` | pc_audio.c | port-as-is |  |
| `DSPCheckMailFromDSP` | `BOOL DSPCheckMailFromDSP(void)` | pc_audio.c | port-as-is |  |
| `DSPReadMailFromDSP` | `u32 DSPReadMailFromDSP(void)` | pc_audio.c | port-as-is |  |
| `DSPSendMailToDSP` | `void DSPSendMailToDSP(u32 mail)` | pc_audio.c | port-as-is |  |
| `DSPAssertInt` | `void DSPAssertInt(void)` | pc_audio.c | port-as-is |  |
| `DSPAddTask` | `void* DSPAddTask(void* task)` | pc_audio.c | port-as-is | Returns the task pointer; the real DSP is never used. |

### Audio — platform-internal

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `pc_audio_start_producer_thread` | `void pc_audio_start_producer_thread(void)` | pc_audio.c | rewrite-for-KOS |  |
| `pc_audio_update_volumes` | `void pc_audio_update_volumes(void)` | pc_audio.c | rewrite-for-KOS |  |
| `pc_audio_get_buffer_fill` ✱ | `int pc_audio_get_buffer_fill(void)` | pc_audio.c | rewrite-for-KOS |  |
| `pc_audio_is_active` ✱ | `int pc_audio_is_active(void)` | pc_audio.c | rewrite-for-KOS |  |
| `pc_audio_shutdown` ✱ | `void pc_audio_shutdown(void)` | pc_audio.c | rewrite-for-KOS |  |

### CARD — memory card

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `CARDInit` | `void CARDInit(void)` | pc_card.c | rewrite-for-KOS |  |
| `CARDMount` | `s32 CARDMount(s32 chan, void* workArea, void* detachCallback)` | pc_card.c | rewrite-for-KOS | PC backs channel 0 with save/card_a/ and channel 1 with save/card_b/, one .gci file each. DC: VMU via KOS vmufs, ~100 KB (PLAN §6). |
| `CARDMountAsync` | `s32 CARDMountAsync(s32 chan, void* workArea, void* detachCb, void* attachCb)` | pc_card.c | rewrite-for-KOS |  |
| `CARDUnmount` | `s32 CARDUnmount(s32 chan)` | pc_card.c | rewrite-for-KOS |  |
| `CARDOpen` | `s32 CARDOpen(s32 chan, const char* fileName, CARDFileInfo_PC* fileInfo)` | pc_card.c | rewrite-for-KOS | ★ CARDFileInfo is 20 bytes (chan/fileNo/offset/length/iBlock) and must match dolphin/card.h; extra PC state lives in a side table keyed by the fileInfo pointer. |
| `CARDClose` | `s32 CARDClose(CARDFileInfo_PC* fileInfo)` | pc_card.c | rewrite-for-KOS |  |
| `CARDCreate` | `s32 CARDCreate(s32 chan, const char* fileName, u32 size, CARDFileInfo_PC* fileInfo)` | pc_card.c | rewrite-for-KOS |  |
| `CARDCreateAsync` | `s32 CARDCreateAsync(s32 chan, const char* fileName, u32 size, void* fileInfo, void* callback)` | pc_card.c | rewrite-for-KOS |  |
| `CARDRead` | `s32 CARDRead(CARDFileInfo_PC* fileInfo, void* buf, s32 length, s32 offset)` | pc_card.c | rewrite-for-KOS |  |
| `CARDReadAsync` | `s32 CARDReadAsync(void* fileInfo, void* buf, s32 length, s32 offset, void* callback)` | pc_card.c | rewrite-for-KOS |  |
| `CARDWrite` | `s32 CARDWrite(CARDFileInfo_PC* fileInfo, const void* buf, s32 length, s32 offset)` | pc_card.c | rewrite-for-KOS | Sector size is 0x2000; the AC land file is 0x72000 bytes (466,944 = ~456 KB) — 4.6x the VMU budget. |
| `CARDWriteAsync` | `s32 CARDWriteAsync(void* fileInfo, const void* buf, s32 length, s32 offset, void* callback)` | pc_card.c | rewrite-for-KOS |  |
| `CARDDelete` | `s32 CARDDelete(s32 chan, const char* fileName)` | pc_card.c | rewrite-for-KOS |  |
| `CARDDeleteAsync` | `s32 CARDDeleteAsync(s32 chan, const char* fileName, void* callback)` | pc_card.c | rewrite-for-KOS |  |
| `CARDGetResultCode` | `s32 CARDGetResultCode(s32 chan)` | pc_card.c | rewrite-for-KOS |  |
| `CARDFreeBlocks` | `s32 CARDFreeBlocks(s32 chan, s32* byteNotUsed, s32* filesNotUsed)` | pc_card.c | rewrite-for-KOS |  |
| `CARDGetSectorSize` | `s32 CARDGetSectorSize(s32 chan, u32* size)` | pc_card.c | rewrite-for-KOS |  |
| `CARDProbeEx` | `s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize)` | pc_card.c | rewrite-for-KOS | Reports card size/sector size; feeds the game's free-block arithmetic. Lying here about a VMU's capacity is how a compressed/segmented scheme gets accepted by game code. |
| `CARDProbe` | `s32 CARDProbe(s32 chan)` | pc_card.c | rewrite-for-KOS |  |
| `CARDCheck` | `s32 CARDCheck(s32 chan)` | pc_card.c | rewrite-for-KOS |  |
| `CARDCheckAsync` | `s32 CARDCheckAsync(s32 chan, void* callback)` | pc_card.c | rewrite-for-KOS |  |
| `CARDGetStatus` | `s32 CARDGetStatus(s32 chan, s32 fileNo, CARDStat* stat)` | pc_card.c | rewrite-for-KOS |  |
| `CARDSetStatus` | `s32 CARDSetStatus(s32 chan, s32 fileNo, CARDStat* stat)` | pc_card.c | rewrite-for-KOS |  |
| `CARDSetStatusAsync` | `s32 CARDSetStatusAsync(s32 chan, s32 fileNo, void* stat, void* callback)` | pc_card.c | rewrite-for-KOS |  |
| `CARDRename` | `s32 CARDRename(s32 chan, const char* oldName, const char* newName)` | pc_card.c | rewrite-for-KOS |  |
| `CARDRenameAsync` | `s32 CARDRenameAsync(s32 chan, const char* oldName, const char* newName, void* callback)` | pc_card.c | rewrite-for-KOS |  |
| `CARDFormat` | `s32 CARDFormat(s32 chan)` | pc_card.c | rewrite-for-KOS |  |
| `CARDFormatAsync` | `s32 CARDFormatAsync(s32 chan, void* callback)` | pc_card.c | rewrite-for-KOS |  |
| `pc_card_scan_for_gci` ✱ | `int pc_card_scan_for_gci(s32 chan, char* out_path, int out_size)` | pc_card.c | rewrite-for-KOS |  |

### m_card — game save manager (replaces src/game/m_card.c)

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `mCD_save_data_aram_malloc` | `void mCD_save_data_aram_malloc(void)` | pc_m_card.c | rewrite-for-KOS | The mail/original-design/diary blocks are ARAM-resident on GC; the PC port malloc()s them instead. On DC these are small enough to stay resident. |
| `mCD_save_data_aram_to_main` | `int mCD_save_data_aram_to_main(void* dst, u32 size, u32 idx)` | pc_m_card.c | rewrite-for-KOS |  |
| `mCD_save_data_main_to_aram` | `int mCD_save_data_main_to_aram(void* src, u32 size, u32 idx)` | pc_m_card.c | rewrite-for-KOS |  |
| `mCD_set_aram_save_data` | `void mCD_set_aram_save_data(void)` | pc_m_card.c | rewrite-for-KOS |  |
| `pc_save_reload` | `int pc_save_reload(void)` | pc_m_card.c | rewrite-for-KOS | PC-only hot-reload of the .gci; keep for the emulator workflow, drop on hardware. |
| `pc_save_check_and_load` | `int pc_save_check_and_load(void)` | pc_m_card.c | rewrite-for-KOS |  |
| `mCD_init_card` | `void mCD_init_card(void)` | pc_m_card.c | rewrite-for-KOS |  |
| `mCD_InitAll` | `void mCD_InitAll(void)` | pc_m_card.c | rewrite-for-KOS |  |
| `mCD_InitGameStart_bg` | `int mCD_InitGameStart_bg(int player_no, int card_private_idx, int start_cond, s32* mounted_chan)` | pc_m_card.c | rewrite-for-KOS |  |
| `mCD_LoadLand` | `void mCD_LoadLand(void)` | pc_m_card.c | rewrite-for-KOS | pc_m_card.c is NOT a Dolphin SDK replacement — it re-implements the GAME's own src/game/m_card.c, which CMake excludes. ~1440 LOC of save/village/travel logic. Reusable on DC above a new storage backend. |
| `mCD_SaveHome_bg` | `int mCD_SaveHome_bg(int param_1, int* chan)` | pc_m_card.c | rewrite-for-KOS |  |
| `mCD_CheckStation_bg` | `int mCD_CheckStation_bg(s32* chan)` | pc_m_card.c | rewrite-for-KOS |  |
| `mCD_SaveStation_NextLand_bg` | `int mCD_SaveStation_NextLand_bg(s32* chan)` | pc_m_card.c | rewrite-for-KOS |  |
| `mCD_SaveStation_Passport_bg` | `int mCD_SaveStation_Passport_bg(s32* chan)` | pc_m_card.c | rewrite-for-KOS |  |
| `mCD_toNextLand` | `void mCD_toNextLand(void)` | pc_m_card.c | rewrite-for-KOS |  |
| `mCD_ReCheckLoadLand` | `void mCD_ReCheckLoadLand(GAME_PLAY* play)` | pc_m_card.c | rewrite-for-KOS |  |
| `mCD_GetThisLandSlotNo` | `int mCD_GetThisLandSlotNo(void)` | pc_m_card.c | rewrite-for-KOS |  |
| `mCD_GetThisLandSlotNo_code` | `int mCD_GetThisLandSlotNo_code(int* player_no, s32* slot_card_results)` | pc_m_card.c | rewrite-for-KOS |  |
| `mCD_GetSaveHomeSlotNo` | `int mCD_GetSaveHomeSlotNo(void)` | pc_m_card.c | rewrite-for-KOS |  |
| `mCD_GetPlayerNum` | `int mCD_GetPlayerNum(void)` | pc_m_card.c | rewrite-for-KOS |  |
| `mCD_GetCardPrivateNameCopy` | `int mCD_GetCardPrivateNameCopy(u8* name, int idx)` | pc_m_card.c | rewrite-for-KOS |  |
| `mCD_CheckCardPlayerNative` | `int mCD_CheckCardPlayerNative(int idx)` | pc_m_card.c | rewrite-for-KOS |  |
| `mCD_CheckPassportFile` | `int mCD_CheckPassportFile(void)` | pc_m_card.c | rewrite-for-KOS |  |
| `mCD_CheckBrokenPassportFile` | `int mCD_CheckBrokenPassportFile(int slot)` | pc_m_card.c | rewrite-for-KOS |  |
| `mCD_EraseBrokenLand_bg` | `int mCD_EraseBrokenLand_bg(int* slot)` | pc_m_card.c | rewrite-for-KOS |  |
| `mCD_EraseLand_bg` | `int mCD_EraseLand_bg(int* slot)` | pc_m_card.c | rewrite-for-KOS |  |
| `mCD_ErasePassportFile_bg` | `int mCD_ErasePassportFile_bg(int slot)` | pc_m_card.c | rewrite-for-KOS |  |
| `mCD_SaveErasePlayer_bg` | `int mCD_SaveErasePlayer_bg(int* slot)` | pc_m_card.c | rewrite-for-KOS |  |
| `mCD_card_format_bg` | `int mCD_card_format_bg(s32 chan)` | pc_m_card.c | rewrite-for-KOS |  |
| `mCD_PrintErrInfo` | `void mCD_PrintErrInfo(gfxprint_t* gfxprint)` | pc_m_card.c | rewrite-for-KOS |  |

### Save byte-swap (GCI interchange)

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `pc_save_bswap` | `void pc_save_bswap(Save_t* save, pc_bswap_dir_t dir)` | pc_save_bswap.c | port-as-is | SH-4 is little-endian exactly like the base port's targets, so the whole GCI byte-swap layer carries over verbatim and Dolphin interchange is preserved. |
| `pc_save_bswap_keep_mail` ✱ | `void pc_save_bswap_keep_mail(mCD_keep_mail_c* mail, pc_bswap_dir_t dir)` | pc_save_bswap.c | port-as-is |  |
| `pc_save_bswap_keep_original` ✱ | `void pc_save_bswap_keep_original(mCD_keep_original_c* orig, pc_bswap_dir_t dir)` | pc_save_bswap.c | port-as-is |  |
| `pc_save_bswap_keep_diary` ✱ | `void pc_save_bswap_keep_diary(mCD_keep_diary_c* diary, pc_bswap_dir_t dir)` | pc_save_bswap.c | port-as-is |  |
| `pc_save_bswap_verify_roundtrip` ✱ | `int pc_save_bswap_verify_roundtrip(const u8* original_be, u32 size)` | pc_save_bswap.c | port-as-is |  |
| `pc_save_bswap_verify_roundtrip_mail` ✱ | `int pc_save_bswap_verify_roundtrip_mail(const u8* original_be, u32 size)` | pc_save_bswap.c | port-as-is |  |
| `pc_save_bswap_verify_roundtrip_original` ✱ | `int pc_save_bswap_verify_roundtrip_original(const u8* original_be, u32 size)` | pc_save_bswap.c | port-as-is |  |
| `pc_save_bswap_verify_roundtrip_diary` ✱ | `int pc_save_bswap_verify_roundtrip_diary(const u8* original_be, u32 size)` | pc_save_bswap.c | port-as-is |  |
| `pc_checksum_be` ✱ | `u16 pc_checksum_be(const u8* data, u32 size, u16 old_checksum)` | pc_save_bswap.c | port-as-is | Big-endian checksum over the save; must stay BE for GCI compatibility regardless of host endianness. |
| `pc_save_bswap_foreigner` ✱ | `void pc_save_bswap_foreigner(mCD_foreigner_c* f, pc_bswap_dir_t dir)` | pc_save_bswap.c | port-as-is |  |

### Runtime asset table (DOL/REL offsets + bswap class)

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `pc_bswap_asset_u16` ✱ | `void pc_bswap_asset_u16(void* data, unsigned int size)` | pc_assets.c | rewrite-for-KOS |  |
| `pc_bswap_asset_u32` ✱ | `void pc_bswap_asset_u32(void* data, unsigned int size)` | pc_assets.c | rewrite-for-KOS |  |
| `pc_bswap_asset_vtx` ✱ | `void pc_bswap_asset_vtx(void* data, unsigned int size)` | pc_assets.c | rewrite-for-KOS | Vtx records are mixed-width (s16 pos + u8 color) — cannot be swapped with a uniform stride, hence the per-asset class. |
| `pc_assets_pal_n64_to_gc` | `void pc_assets_pal_n64_to_gc(u16* pal, int count)` | pc_assets.c | rewrite-for-KOS |  |
| `pc_load_asset` | `void pc_load_asset(const char* bin_path, void* dest, unsigned int size, unsigned int rom_off, int rom_src, int swap_type)` | pc_assets.c | rewrite-for-KOS | ★ Generated 30k-line dispatch table (~2500 assets -> DOL/REL offset + per-asset byte-swap class), produced by pc/tools/gen_runtime_assets.py. The DC build keeps the table but the source moves from a runtime ISO parse to files on /cd. |
| `pc_assets_init` ✱ | `void pc_assets_init(void)` | pc_assets.c | rewrite-for-KOS | Called from main() BEFORE ac_entry(); populates every .inc asset the linked game code references. |

### Disc-image reader (moves to host tools/)

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `pc_disc_init` ✱ | `int pc_disc_init(void)` | pc_disc.c | drop | GC disc/CISO/GCM/FST/Yaz0 reader. On DC this runs OFFLINE in tools/ at disc-build time; nothing of it ships in 1ST_READ.BIN. |
| `pc_disc_is_open` ✱ | `int pc_disc_is_open(void)` | pc_disc.c | drop |  |
| `pc_disc_last_error` ✱ | `int pc_disc_last_error(void)` | pc_disc.c | drop |  |
| `pc_disc_game_id` ✱ | `const char* pc_disc_game_id(void)` | pc_disc.c | drop |  |
| `pc_disc_find_file` ✱ | `int pc_disc_find_file(const char* path, u32* disc_offset, u32* file_size)` | pc_disc.c | drop |  |
| `pc_disc_read` ✱ | `int pc_disc_read(u32 offset, void* dest, u32 size)` | pc_disc.c | drop |  |
| `pc_disc_extract_dol` ✱ | `u8* pc_disc_extract_dol(void)` | pc_disc.c | drop |  |
| `pc_disc_extract_rel` ✱ | `u8* pc_disc_extract_rel(void)` | pc_disc.c | drop |  |
| `pc_disc_shutdown` ✱ | `void pc_disc_shutdown(void)` | pc_disc.c | drop |  |

### Settings

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `pc_settings_cull_limit_xz` | `float pc_settings_cull_limit_xz(float cull_distance, float cull_radius)` | pc_settings.c | rewrite-for-KOS |  |
| `pc_settings_save` | `void pc_settings_save(void)` | pc_settings.c | rewrite-for-KOS |  |
| `pc_settings_get_nes_aspect` ✱ | `int pc_settings_get_nes_aspect(void)` | pc_settings.c | rewrite-for-KOS |  |
| `pc_settings_reset_controllers` ✱ | `void pc_settings_reset_controllers(void)` | pc_settings.c | rewrite-for-KOS |  |
| `pc_settings_autodetect_resolution` ✱ | `void pc_settings_autodetect_resolution(void)` | pc_settings.c | rewrite-for-KOS |  |
| `pc_settings_apply` | `void pc_settings_apply(void)` | pc_settings.c | rewrite-for-KOS |  |
| `pc_settings_load` | `void pc_settings_load(void)` | pc_settings.c | rewrite-for-KOS |  |

### Profiler

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `pc_prof_now_us` ✱ | `unsigned long long pc_prof_now_us(void)` | pc_prof.c | rewrite-for-KOS |  |
| `pc_prof_report` ✱ | `void pc_prof_report(const char* tag, int id, unsigned long long t0_us)` | pc_prof.c | rewrite-for-KOS |  |

### Famicom (NES emu) stubs

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `famicom_1frame` | `void famicom_1frame(void)` | pc_stubs.c | port-as-is |  |
| `famicom_cleanup` | `int famicom_cleanup(void)` | pc_stubs.c | port-as-is |  |
| `famicom_external_data_save` | `int famicom_external_data_save(void)` | pc_stubs.c | port-as-is |  |
| `famicom_external_data_save_check` | `int famicom_external_data_save_check(void)` | pc_stubs.c | port-as-is |  |
| `famicom_getErrorChan` | `int famicom_getErrorChan(void)` | pc_stubs.c | port-as-is |  |
| `famicom_get_disksystem_titles` | `int famicom_get_disksystem_titles(int* n_games, char* title_name_bufp, int namebuf_size)` | pc_stubs.c | port-as-is |  |
| `famicom_init` | `int famicom_init(int rom_idx, void* malloc_info, int player_no)` | pc_stubs.c | port-as-is | The NES emulator core is PPC assembly and is excluded from the build; these stubs satisfy the linker. NES is a declared non-goal for DC. |
| `famicom_internal_data_load` | `int famicom_internal_data_load(void)` | pc_stubs.c | port-as-is |  |
| `famicom_internal_data_save` | `int famicom_internal_data_save(void)` | pc_stubs.c | port-as-is |  |
| `famicom_mount_archive` | `void famicom_mount_archive(void)` | pc_stubs.c | port-as-is |  |
| `famicom_mount_archive_end_check` | `int famicom_mount_archive_end_check(void)` | pc_stubs.c | port-as-is |  |
| `famicom_rom_load_check` | `int famicom_rom_load_check(void)` | pc_stubs.c | port-as-is |  |
| `famicom_setCallback_getSaveChan` | `void famicom_setCallback_getSaveChan(void* proc)` | pc_stubs.c | port-as-is |  |

### libultra stubs

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `osContGetQuery` | `void osContGetQuery(void* status)` | pc_stubs.c | port-as-is |  |
| `osContGetReadData` | `void osContGetReadData(void* pad)` | pc_stubs.c | port-as-is |  |
| `osContInit` | `s32 osContInit(void* mq, u8* pattern_p, void* status)` | pc_stubs.c | port-as-is | libultra controller API — the decomp calls it but the GC build never uses it. Stubs compile unchanged. |
| `osContSetCh` | `s32 osContSetCh(u8 num_controllers)` | pc_stubs.c | port-as-is |  |
| `osContStartQuery` | `s32 osContStartQuery(void* mq)` | pc_stubs.c | port-as-is |  |
| `osContStartReadData` | `s32 osContStartReadData(void* mq)` | pc_stubs.c | port-as-is |  |
| `osDestroyThread` | `void osDestroyThread(void* t)` | pc_stubs.c | port-as-is |  |
| `osGetThreadId` | `s32 osGetThreadId(void* thread)` | pc_stubs.c | port-as-is |  |
| `osSetTimer` | `int osSetTimer(void* t, s64 countdown, s64 interval, void* mq, void* msg)` | pc_stubs.c | port-as-is |  |
| `osSyncPrintf` | `void osSyncPrintf(const char* fmt, ...)` | pc_stubs.c | port-as-is |  |

### GBA link stubs

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `GBAInit` | `void GBAInit(void)` | pc_stubs.c | port-as-is |  |
| `GBAGetStatus` | `s32 GBAGetStatus(s32 chan, u8* status)` | pc_stubs.c | port-as-is |  |
| `GBAGetProcessStatus` | `s32 GBAGetProcessStatus(s32 chan, u8* percentp)` | pc_stubs.c | port-as-is |  |
| `GBARead` | `s32 GBARead(s32 chan, u8* dst, u8* status)` | pc_stubs.c | port-as-is |  |
| `GBAWrite` | `s32 GBAWrite(s32 chan, u8* src, u8* status)` | pc_stubs.c | port-as-is |  |
| `GBAReset` | `s32 GBAReset(s32 chan, u8* status)` | pc_stubs.c | port-as-is |  |
| `GBAJoyBootAsync` | `s32 GBAJoyBootAsync(s32 chan, s32 palette_color, s32 palette_speed, u8* programp, s32 length, u8* status, void* callback)` | pc_stubs.c | port-as-is |  |

### Misc stubs

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `__abs` | `int __abs(int x)` | pc_stubs.c | port-as-is |  |
| `_strip` ✱ | `void _strip(float x)` | pc_stubs.c | port-as-is |  |
| `vaprintf` | `int vaprintf(void* func, const char* fmt, va_list ap)` | pc_stubs.c | port-as-is |  |

### JSystem C++ vtable fills

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `JSUOutputStream::~JSUOutputStream` ✱ | `JSUOutputStream::~JSUOutputStream()` | pc_stubs_cpp.cpp | port-as-is |  |
| `JSUOutputStream::skip` ✱ | `int JSUOutputStream::skip(s32 amount)` | pc_stubs_cpp.cpp | port-as-is |  |
| `JSURandomOutputStream::getAvailable` ✱ | `int JSURandomOutputStream::getAvailable() const` | pc_stubs_cpp.cpp | port-as-is |  |
| `JSURandomOutputStream::skip` ✱ | `int JSURandomOutputStream::skip(s32 amount)` | pc_stubs_cpp.cpp | port-as-is |  |
| `JKRHeap::destroy` ✱ | `void JKRHeap::destroy()` | pc_stubs_cpp.cpp | port-as-is | Declared in the decomp headers but never compiled; C++ vtables need a body. Compiles unchanged for sh-elf. |
| `JKRTask::searchBlank` ✱ | `JKRTask::Request* JKRTask::searchBlank()` | pc_stubs_cpp.cpp | port-as-is |  |

### glibc symbol-version compat

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `__isoc23_strtol` ✱ | `long int __isoc23_strtol(const char *nptr, char **endptr, int base)` | glibc_compat.c | drop | glibc 2.38+ symbol-version shim so the ARM binary runs on older glibc. Irrelevant with newlib — drop. |
| `__isoc23_sscanf` ✱ | `int __isoc23_sscanf(const char *str, const char *fmt, ...)` | glibc_compat.c | drop |  |

### PC-only features

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `pc_keybindings_save` ✱ | `void pc_keybindings_save(void)` | pc_keybindings.c | drop |  |
| `pc_keybindings_reset` ✱ | `void pc_keybindings_reset(void)` | pc_keybindings.c | drop |  |
| `pc_keybindings_uses_gamepad` ✱ | `int pc_keybindings_uses_gamepad(void)` | pc_keybindings.c | drop |  |
| `pc_keybinding_label` ✱ | `const char* pc_keybinding_label(int idx)` | pc_keybindings.c | drop |  |
| `pc_keybinding_ptr` ✱ | `PCInputCode* pc_keybinding_ptr(int idx)` | pc_keybindings.c | drop |  |
| `pc_keybindings_load` ✱ | `void pc_keybindings_load(void)` | pc_keybindings.c | drop |  |
| `pc_model_viewer_init` | `void pc_model_viewer_init(GAME* game)` | pc_model_viewer.c | drop |  |
| `pc_model_viewer_cleanup` ✱ | `void pc_model_viewer_cleanup(GAME* game)` | pc_model_viewer.c | drop |  |
| `pc_overlay_init` ✱ | `void pc_overlay_init(void)` | pc_overlay.c | drop |  |
| `pc_overlay_shutdown` ✱ | `void pc_overlay_shutdown(void)` | pc_overlay.c | drop |  |
| `pc_overlay_update` ✱ | `void pc_overlay_update(double fps, double speed)` | pc_overlay.c | drop |  |
| `pc_overlay_menu_toggle` ✱ | `void pc_overlay_menu_toggle(void)` | pc_overlay.c | drop |  |
| `pc_overlay_boot_splash` ✱ | `void pc_overlay_boot_splash(const char* msg)` | pc_overlay.c | drop |  |
| `pc_overlay_boot_error_frame` ✱ | `void pc_overlay_boot_error_frame(const char* const* lines, int n_lines)` | pc_overlay.c | drop |  |
| `pc_overlay_draw` ✱ | `void pc_overlay_draw(void)` | pc_overlay.c | drop |  |
| `pc_texture_pack_init` ✱ | `void pc_texture_pack_init(void)` | pc_texture_pack.c | drop |  |
| `pc_texture_pack_preload_all` ✱ | `void pc_texture_pack_preload_all(void)` | pc_texture_pack.c | drop |  |
| `pc_texture_pack_shutdown` ✱ | `void pc_texture_pack_shutdown(void)` | pc_texture_pack.c | drop |  |
| `pc_texture_pack_active` ✱ | `int pc_texture_pack_active(void)` | pc_texture_pack.c | drop |  |
| `pc_texture_pack_lookup` ✱ | `GLuint pc_texture_pack_lookup(const void* data, int data_size, int w, int h, unsigned int fmt, const void* tlut_data, int tlut_entries, int tlut_is_be, int* out_w, int* out_h)` | pc_texture_pack.c | drop |  |
| `pc_typing_queue_clear` ✱ | `void pc_typing_queue_clear(void)` | pc_typing.c | drop |  |
| `pc_typing_queue_push` ✱ | `void pc_typing_queue_push(int code)` | pc_typing.c | drop |  |
| `pc_typing_queue_pop` | `int pc_typing_queue_pop(int* out)` | pc_typing.c | drop |  |
| `pc_utf8_to_game_code` ✱ | `int pc_utf8_to_game_code(const char* text)` | pc_typing.c | drop |  |
| `pc_typing_handle_event` ✱ | `void pc_typing_handle_event(const SDL_Event* event)` | pc_typing.c | drop |  |
| `pc_typing_update` ✱ | `void pc_typing_update(void)` | pc_typing.c | drop |  |

### OS — console settings / misc stubs

| symbol | signature | pc/ file | DC disposition | notes |
|---|---|---|---|---|
| `OSGetFontEncode` | `u16 OSGetFontEncode(void)` | pc_stubs.c | port-as-is |  |
| `OSGetProgressiveMode` | `u32 OSGetProgressiveMode(void)` | pc_stubs.c | port-as-is |  |
| `OSSetProgressiveMode` | `void OSSetProgressiveMode(u32 on)` | pc_stubs.c | port-as-is |  |
| `OSGetSoundMode` | `u32 OSGetSoundMode(void)` | pc_stubs.c | port-as-is |  |
| `OSSetSoundMode` | `void OSSetSoundMode(u32 mode)` | pc_stubs.c | port-as-is |  |
| `OSProtectRange` | `void OSProtectRange(u32 chan, void* addr, u32 nBytes, u32 control)` | pc_stubs.c | port-as-is |  |
| `OSSetErrorHandler` | `void* OSSetErrorHandler(u16 error, void* handler)` | pc_stubs.c | port-as-is |  |
| `OSReportEnable` | `void OSReportEnable(void)` | pc_stubs.c | port-as-is |  |

---

## 6. Global variables the platform layer also defines

Link-level obligations that are not functions. Verified by scanning column-0
definitions in `pc/src/*.c` (excluding the generated `pc_assets.c`).

| symbol | type | file | referenced by game code | DC disposition |
|---|---|---|---|---|
| `__VIRegs[59]`, `__PIRegs[12]`, `__MEMRegs[64]`, `__DSPRegs[32]`, `__DIRegs[16]`, `__SIRegs[0x100]`, `__EXIRegs[0x40]`, `__AIRegs[8]` | `volatile u16/u32[]` | pc_misc.c | yes (decomp reads/writes MMIO shadows) | port-as-is — zeroed dummies; **do not** point these at real DC hardware |
| `__OSPhysicalMemSize`, `__OSSimulatedMemSize` | `u32` = 24 MB | pc_misc.c | yes | rewrite — must match the DC arena size |
| `__OSBusClock` (162 MHz), `__OSCoreClock` (486 MHz) | `u32` | pc_misc.c | yes | port-as-is — these are the *GameCube* clocks the tick rate derives from; changing them changes game timing |
| `__OSTVMode`, `__OSDeviceCode` | `volatile` | pc_misc.c | yes | port-as-is |
| `GXNtsc480IntDf`, `GXNtsc480Int`, `GXMpal480IntDf`, `GXPal528IntDf`, `GXEurgb60Hz480IntDf` | `u8[64]` all-zero | pc_misc.c | yes — `JW_Init` passes `&GXNtsc480IntDf` to `JFWDisplay::createManager` and `JFWSystem::CSetUpParam::renderMode` points at it | rewrite — DC must fill in at least width/height/efbHeight; `JFWSystem::init()` branches on `renderMode->efbHeight < 300` |
| `BaseModule`, `__OSModuleList`, `__OSStringTable` | REL module state | pc_misc.c | yes (`boot.c` walks `BaseModule`) | stub-and-log (NULL) |
| `DiskID[32]` | `u8[]` | pc_misc.c | yes | port-as-is |
| `__gUnkThread1`, `__gCurrentThread`, `__gUnknown800030C0[2]`, `__gUnknown800030E3` | low-memory globals | pc_misc.c | yes | port-as-is |
| `__OSCurrHeap` | `volatile int` = −1 | pc_os.c | yes (`OSAlloc` macro) | port-as-is |
| `osAppNMIBuffer[64]` | `s32[]` | pc_os.c | yes — the zurumode/reset flag words | port-as-is |
| `__osResetSwitchPressed`, `osShutdown` | `int` | pc_os.c | yes | port-as-is |
| `pc_arena_base`, `pc_arena_end` | `u8*` | pc_os.c | yes — seg2k0 pointer heuristic | rewrite (§3.1, PLAN §11.6) |
| `pc_OSBusClock`, `pc_OSCoreClock` | `u32` | pc_os.c | ✱ | port-as-is |
| `pc_image_base`, `pc_image_end` | `unsigned int` | pc_main.c | yes — seg2k0 | rewrite — ELF program headers → KOS's fixed `0x8C010000` load address |
| `g_gx` | `PCGXState` (≈3.2 MB, incl. the 65536×48 B vertex buffer) | pc_gx.c | ✱ | rewrite — shrinks to a PVR-native 32-byte vertex record |
| `pc_gx_draw_call_count`, `pc_gx_prim_draws[5]`, `pc_gx_merged_batches`, `pc_gx_culled_draws`, `pc_gx_flush_reason[18]`, `pc_gx_flush_time_us`, `pc_gx_texload_time_us` | diagnostics | pc_gx.c | ✱ | port-as-is (keep — this is how batching regressions get caught) |
| `pc_emu64_frame_cmds/_crashes/_noop_cmds/_tri_cmds/_vtx_cmds/_dl_cmds/_cull_visible/_cull_rejected` | `int` | pc_gx.c | yes (emu64 increments them) | port-as-is |
| `pc_gx_state_dedup`, `pc_gx_uniform_shadow` | `int` kill switches | pc_gx.c | ✱ | port-as-is — PLAN requires every optimization keep a kill switch |
| `pc_frame_counter` | `u32` | pc_vi.c | ✱ | port-as-is |
| `pc_save_loaded` | `int` | pc_m_card.c | yes | port-as-is |
| `g_pc_settings` | `PCSettings` | pc_settings.c | yes (`pc_settings_cull_limit_xz`) | rewrite/trim |
| `g_pc_running`, `g_pc_verbose`, `g_pc_frameskip_active`, `g_pc_fps_target`, `g_pc_render_w/h`, `g_pc_window_w/h`, `g_pc_widescreen_stretch`, `g_pc_zoom`, `g_pc_scale_mode`, `g_pc_no_framelimit`, `g_pc_time_override`, `g_pc_min_override`, `g_pc_sec_override` | platform globals | pc_main.c | mixed | rewrite/trim |
| `g_pc_window`, `g_pc_gl_context` | SDL handles | pc_main.c | ✱ | drop |
| `g_pc_keybindings`, `g_pc_typing_mode`, `g_pc_editor_active`, `g_pc_typing_queue`, `g_pc_menu_open`, `g_pc_model_viewer*` | PC-only feature state | pc_keybindings/typing/overlay/model_viewer | ✱ | drop for M1 |
| `s_tlut_first_word[16]` | `u16[]` | pc_gx_texture.c | ✱ | rewrite (TLUT cache key) |
| `pc_gx_tev_last_locs`, `pc_gx_tev_last_gens` | GLSL uniform cache | pc_gx_tev.c | ✱ | drop |

---

## 7. Known gaps / not verified in this pass

- **`bzero` / `bcopy`**: `pc_misc.c` only defines them under `_WIN32`
  (glibc supplies them otherwise). Whether KOS/newlib for sh-elf supplies
  both was **not verified**. `boot.c` calls both. If missing, add them to
  `dc/`.
- **KOS API names** used above (`dcache_flush_range`, `dcache_inval_range`,
  `rtc_unix_secs`, `timer_ns_gettime64`, `vmufs`, `snd_stream`,
  `pvr_scene_finish`) are from `kb/research-dreamcast.md` and general KOS
  knowledge; **none were checked against a KOS tree in this pass** (no
  toolchain installed yet). Confirm during M0.
- **`clock_gettime(CLOCK_MONOTONIC)`** availability under KOS/newlib is
  unverified (`pc_prof.c` needs it).
- The symbol extraction is a **column-0 definition parser**, not a compiler.
  It found 698 non-static definitions across 27 `.c`/`.cpp` files. Spot
  checks against `pc_os.c`, `pc_aram.c`, `pc_dvd.c`, `pc_vi.c`, `pc_audio.c`
  (read in full) matched. A handful of `static` functions declared with an
  unusual layout could in principle be misclassified as non-static; the
  reverse (missing a real export) was not observed but cannot be excluded
  without a real link.
- **Reference counting** ("✱") is a whole-word identifier match over `src/`
  and `include/` including comments and `#ifdef`-disabled code. It over-
  counts (a symbol named in a dead `#else` branch counts as referenced) and
  is therefore a *safe* signal: ✱ genuinely means "no mention anywhere".
- `EXI*`, `SI*`, `DB*`, and most `PPC*` are marked `drop` because a grep over
  compiled `src/` found **0** call sites. The two live PPC paths are
  `PPCMfmsr`/`PPCMtmsr` (`JUTException.cpp`, MSR interrupt masking) and
  `PPCSync` (`emu64.c:5912`, `dspdriver.c:373`). Dropping the rest is safe
  only if nothing in `src/data/` references them — that subtree was included
  in the reference scan, so this is believed safe.
- The DC disposition column is a **design proposal**, not a measurement. It
  is the recommendation of this recon pass and should be revised as M1/M2
  measurements come in.
