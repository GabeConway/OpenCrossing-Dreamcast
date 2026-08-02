# Fitting in 16 MB — what the rest of the Dreamcast memory map offers

VRAM as a store (addressing, the 16-bit write rule, how much is actually free,
the `NOLOAD` placement mechanism), AICA sound RAM behind G2, store queues,
operand-cache RAM, and the low 64 KB (§5). Read before proposing to move an
array off main RAM. Part of `kb/research-size-reduction.md`, whose stub maps every § to its file.

Tags: **[M]** measured today against the real DC ELF, **[S]** sourced to a URL,
**[D]** derived arithmetic, **[?]/[UNVERIFIED]** not confirmed.

⚠️ `kb/levers.md` L3 re-costed every estimate in this document against
the real ELF: **every one was wrong, most by a lot, and two of the stated
mechanisms were impossible.** Use `kb/levers.md` for numbers; use this
document for the reasoning and the sources.
Sources cited here are indexed in `kb/research-size-plan.md` §8.

## 5. What else the Dreamcast memory map offers

### 5.1 VRAM (8 MB) as a store — real, with sharp edges

**Addressing.** VRAM is physical area 1, `0x04000000`–`0x07FFFFFF`, 8 MB in two
independent 4 MB modules. Two aliases of the same silicon:
- **64-bit area** at `0x05000000` (P2/uncached: `0xA5000000`) — the two modules
  interleave every 4 bytes, so consecutive access alternates modules and is
  faster. This is where textures live.
- **32-bit area** at `0x04000000` (`0xA4000000`) — modules sequential; the
  framebuffer lives here.
- Mirrors at `0x06000000` / `0x07000000`.

<https://dreamcast.wiki/VRAM>, <https://mc.pp.se/dc/memory.html> **[S]**

**Rules you must obey.**
- "there is no restriction on what sizes may be used for read and write
  operations" in either area — but the widely repeated hardware rule is that
  **8-bit writes are not possible** (16-bit minimum). Treat any byte-granular
  array as unusable in VRAM. **[S]/[?]** — dreamcast.wiki states the "no
  restriction" line while the Sega hardware outline states the 16-bit-minimum
  write rule; I could not reconcile these two sources, so **assume 16-bit
  minimum** and design around it.
- Store queues write *to* VRAM efficiently: 32-byte units, 32-byte-aligned
  destination, 4-byte-aligned source (8 for best speed), size a multiple of 32.
  `pvr_sq_*` variants cannot be used concurrently with PVR DMA.
  <https://kos-docs.dreamcast.wiki/group__store__queues.html> **[S]**
- **Reads are the problem.** SH-4 reads from VRAM are consistently described as
  "much slower than accessing main RAM"; the VRAM bus is 64-bit @ 100 MHz
  (~800 MB/s aggregate, shared with the PVR's rasteriser, texture fetch, and
  display scan-out), and the SH-4 is a distant second-priority master on it.
  <https://www.copetti.org/writings/consoles/dreamcast/>,
  <https://dcemulation.org/phpBB/viewtopic.php?t=96364> **[S]**
  I could **not** find a citable measured SH-4↔VRAM read figure. Any number you
  see quoted, including "~520 MB/s SQ bandwidth", is **[UNVERIFIED]** — measure
  it at M1 before budgeting on it.

**How much is actually free.** Not 8 MB. The community answer is blunt: "when
you're using the PVR for rendering, about half of VRAM is off limits due to the
fact that it's needed for the PVR's buffers. Even when you're not using the
PVR, the framebuffers still reside in VRAM."
<https://dcemulation.org/phpBB/viewtopic.php?t=96364> **[S]**
Arithmetic for our case **[D]**: 2 × 640×480×16bpp framebuffers = 1,228,800 B,
plus TA vertex/OPB buffers (KOS `pvr_init` sizes these; typically 1–2 MB), plus
every texture the game needs. Animal Crossing is texture-heavy. **Budget zero
spare VRAM until `pvr_mem_available()` is measured on real hardware.**

**The mechanism, and it is clean.** You do not need a special API to place data
in VRAM — the KOS linker script already demonstrates the exact pattern for
off-main-RAM `NOLOAD` placement:

```ld
/* utils/ldscripts/shlelf.xc */
_end = .; PROVIDE (end = .);
.ocram 0x7c001000 (NOLOAD) :
{
  *(.ocram)
  . = . > 0x2000 ? 0x2000 : .;   /* error if > 8 KB of operand-cache RAM */
}
```
<https://github.com/KallistiOS/KallistiOS/blob/master/utils/ldscripts/shlelf.xc> **[S]**

A `.vram_bss 0xa5000000 (NOLOAD)` output section plus
`__attribute__((section(".vram_bss")))` on chosen arrays is **a placement
change, not a codegen change** — GCC emits identical instructions; only the
symbol's address differs. Two hard requirements: (a) the range must be carved
out of KOS's PVR allocator (`pvr_mem_malloc` hands out 32-byte-aligned blocks
from a dlmalloc pool initialised by `pvr_mem_initialize`
<https://kos-docs.dreamcast.wiki/group__pvr__mem__mgmt.html> **[S]**) or you
will get silent corruption; (b) the arrays must be 16-bit-or-wider access and
must tolerate uncached, high-latency reads.

**Best candidates** (write-mostly, read by hardware, never byte-indexed):
`prbuf` (1,228,800 B — but the *right* answer is a real PVR render target, not
a hand-placed buffer), `emu64 texture_buffer_data` (524,288 B — it is a texture
decode scratch buffer whose output goes to VRAM anyway). Worst candidates:
anything the game `memcpy`s out of every frame, anything with `u8` element type,
anything holding pointers the game dereferences.

### 5.2 AICA sound RAM (2 MB) — a real ARAM analogue, but DMA-only

`SPU_RAM_BASE = 0x00800000`, uncached alias `0xA0800000`. KOS exposes it as a
block device, not as memory: `spu_memload()`, `spu_memload_sq()`,
`spu_memload_dma()`, `spu_memread()`, `spu_memset()`, `spu_dma_transfer()`.
<https://github.com/KallistiOS/KallistiOS/blob/master/kernel/arch/dreamcast/include/dc/spu.h> **[S]**

Why it is not a `.bss` target. It sits behind the G2 bus, which KOS's own header
describes as "notoriously picky … You have to be careful to use the right access
size for whatever you're working with. Also you can't be doing PIO and DMA at
the same time. Finally, there's a FIFO to contend with when you're doing PIO
stuff as well. Generally, G2 is a pain in the rear."
<https://github.com/KallistiOS/KallistiOS/blob/master/kernel/arch/dreamcast/include/dc/g2bus.h> **[S]**
G2 is 16-bit @ 25 MHz with a real transfer rate around **40 MB/s (19 clk/32 B)**
per the Sega hardware outline
<https://segaretro.org/images/8/8b/Dreamcast_Hardware_Specification_Outline.pdf> **[S]**

So: **you cannot put a C array there and let the compiler address it.** You can
use it exactly the way the GameCube used ARAM — an explicit `ARStartDMA`-shaped
block store. That is precisely what `dc/src/dc_aram.c` is already modelling, and
its header already says the sound half (8.44 MB) "DIES — samples move to AICA
sound RAM / disc streaming."

Capacity check **[D]**: `audiorom.img` is 8,300,384 B, four times AICA RAM. AICA
can hold the *resident* sample set only; the rest streams from disc. There is no
spare AICA RAM for graph data. Budget **0 MB** of AICA for the size problem, and
count it as an audio-subsystem win instead.

### 5.3 Store queues — a transfer mechanism, not storage

Two 32-byte queues at `0xE0000000`/`0xE0000020`, committed with a `pref`. They
are how you *get* data into VRAM/AICA fast; they hold nothing. KOS API:
`sq_cpy`, `sq_fast_cpy`, `sq_set*`, `sq_lock`/`sq_unlock`, `pvr_sq_load`.
"DMA is faster for transactions which are consistently large; however, the store
queues tend to have better performance and have less configuration overhead when
bursting smaller chunks."
<https://kos-docs.dreamcast.wiki/group__store__queues.html> **[S]**
Relevant to §5.1 and to the asset pool's fill path; **saves 0 bytes on its own.**

### 5.4 Operand-cache RAM (8 KB) — real, tiny

Half the SH-4 data cache can be re-purposed as directly addressable OCRAM,
mapped at `0x7c001000`, and KOS's linker script already provides a `.ocram`
NOLOAD section with a build-time size assert (see §5.1 listing).
<https://dreamcast.wiki/Useful_programming_tips> **[S]**
8,192 B of genuinely off-main-RAM storage — 0.06 % of the deficit, and it costs
you half the D-cache. Mention it only so it is not proposed as a solution.

### 5.5 The low 64 KB (`0x8c000000`–`0x8c010000`)

`LOAD_OFFSET` is overridable in the KOS linker script
(`LOAD_OFFSET = DEFINED(LOAD_OFFSET) ? LOAD_OFFSET : 0x8c010000`) **[S]**, and
KOS's own `page_phys_base` is `0x8c010000` with
`arch.h:297` validating pointers as `ptr >= 0x8c010000 && ptr < _arch_mem_top`.
That region holds BIOS-installed syscall vectors and boot state. 64 KB for a
class of failure that will not reproduce in Flycast. **Do not.**

---
