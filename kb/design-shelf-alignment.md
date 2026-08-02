# sh-elf compile hazards — alignment hazards and miscompile triage

⚠️ **Two of this document's instructions violate `CLAUDE.md` §1 and must not be
followed as written.** §8 Step 0 says to add a definition to
`src/static/JSystem/JUtility/JUTFont.cpp`, and §2.4 says to add an `#include`
to `include/JSystem/JUtility/JUTFont.h`. **`src/` is never edited to make
something compile** — every compat fix goes in `dc/include/dc_prelude.h`, which
is force-included, and all 3917 TUs build that way today with zero exclusions.
Read those steps as "here is the collision", not as "here is the fix".
Similarly, §3.4 and §9 recommend building at `-O2`; codegen flags are banned by
user directive. (Noted 2026-08-02 while splitting this document.)

SH-4 vs ARM alignment exposure, the KOS address-error triage hook, the ranked
list of hazard sites in `src/`, and the fix idiom (§4); then the step-by-step
triage procedure for a miscompile (§8). Read before debugging a fault, or
before auditing a pointer cast. Part of `kb/design-shelf-hazards.md`, whose stub maps every § to its file.

Written 2026-08-01. **Everything marked VERIFIED below was measured**, not
reasoned about: `sh-elf-gcc 9.3.0` from the `einsteinx2/dcdev-kos-toolchain`
image (KOS `525cbda`, newlib 3.3.0), run against this repo's actual `src/`
tree. Items marked UNVERIFIED say so explicitly.

> **Toolchain caveat.** All compiler measurements used **GCC 9.3.0**. PLAN §8
> targets dc-chain **GCC 14+** per sm64-dc. ABI facts (`__BIGGEST_ALIGNMENT__`,
> `__SIZEOF_DOUBLE__`, char signedness, absence of 64-bit load/store) are
> stable across GCC versions. Codegen-detail claims (store merging, memcpy
> inlining) must be re-checked once the real M0 container exists.

> ## ⚠️ Superseded in part — read this before trusting anything below
>
> **Updated 2026-08-01.** The real M0 container now exists and is *not* the
> image this document measured: `opencrossing-dc:sdk` is **GCC 15.2.0 /
> newlib 4.6.0.20260123 / KOS 2.3.0 (`1c6398f9`)**, versus GCC 9.3.0 / newlib
> 3.3.0 / KOS `525cbda` here. All 3917 TUs compile and link against it with
> **zero exclusions and no edits to `src/`**, so this document's exclusion
> lists are pessimistic. Known divergences:
>
> - **`-fno-builtin` (§3.1) is falsified** — it was marked "(VERIFIED)" as KOS
>   convention and it breaks the link. See the flag table.
> - **The §2.3 header-collision scan missed the two collisions that actually
>   bit us**, because newer KOS/newlib headers introduce them: KOS
>   `dc/fmath.h:109`'s `static inline float fsqrt(float)` versus the decomp's
>   `math64.h:34` `#define fsqrt(x) sqrtf(x)`, and POSIX `link()` versus the
>   decomp's `typedef struct link_ link` arriving through `<stdio.h>` →
>   `<sys/stdio.h>` → `<unistd.h>`. Both are fixed in
>   `dc/include/dc_prelude.h`; the second cannot be fixed with a blanket
>   `-Dlink=`, which renames both sides.
> - **Char signedness (§2.x):** this image defaults to **SIGNED**, so
>   `-fsigned-char` is belt-and-braces, not load-bearing.
> - **Optimization flags anywhere below are moot** — `src/` builds at `-O0`
>   by user directive (CLAUDE.md, PLAN §3.2).
>
> Treat the ABI facts as durable and every codegen-detail claim as unverified
> until re-measured on GCC 15.2.

---

## 4. Alignment hazards — the actual risk

### 4.1 SH-4 vs ARM: the exposure is *different*, and larger

VERIFIED compiler facts (probe programs, `sh-elf-gcc -O2 -S`):

* `__BIGGEST_ALIGNMENT__` is **4**. `__alignof__(long long)` = 4,
  `__alignof__(double)` = 4. `offsetof(struct{char c; long long ll;}, ll)` = 4.
* **SH-4 has no 64-bit integer load/store.** `*(u64*)a != *(u64*)b` on a
  `Gfx`-shaped union compiles to *two* `mov.l` at offsets 0 and 4 — never a
  paired 8-byte access. The **ARM LDRD class simply does not exist here.**
* `sizeof(Gfx)` = 8 but `__alignof__(Gfx)` = **4** (VERIFIED against this
  repo's `include/PR/gbi.h`; the `long long force_structure_alignment` member
  no longer forces 8). `sizeof(Vtx)` = 16, align 4, `offsetof(Vtx_t,cn)` = 12.
* GCC **does not** merge four adjacent `mov.b` stores into a `mov.l`, and does
  not merge two adjacent `mov.w` loads into a `mov.l`, at `-O2`. It respects
  `STRICT_ALIGNMENT`. (ARM's store-merging under NEON is where the base port's
  `-O1` trouble plausibly came from.)
* `__builtin_memcpy(d,s,4)` with unknown alignment emits an out-of-line
  `memcpy` call — byte-safe.
* `__attribute__((packed))` and `__attribute__((aligned(1)))` produce
  **byte-wise load/shift/or sequences** — i.e. GCC SH *does* have a safe
  unaligned-access idiom, it just needs to be asked.

**But**: ARMv7-A with `SCTLR.A=0` (the Linux default, hence the base port)
services unaligned `LDR`/`LDRH`/`STR`/`STRH` **in hardware, silently**. x86
tolerates everything. SH-4 raises a CPU address error on **any** misaligned
`mov.w` or `mov.l`. There is no `movua.l` on SH-4 (that is SH-4A).

> **Therefore: every 16- and 32-bit unaligned access in this codebase has
> never been exercised by any existing port.** The base repo's clean ARM record
> is *not* evidence that these sites are aligned. This is the single largest
> unknown in M1/M2, larger than the `-O2` question.

### 4.2 Triage hook — VERIFIED available

KOS exposes exactly what is needed:

```c
/* kernel/arch/dreamcast/include/arch/irq.h */
#define EXC_DATA_ADDRESS_READ   0x00e0
#define EXC_DATA_ADDRESS_WRITE  0x0100
typedef void (*irq_handler)(irq_t source, irq_context_t *context);
int irq_set_handler(irq_t source, irq_handler hnd);
int irq_set_global_handler(irq_handler hnd);
#define CONTEXT_PC(c)  ((c).pc)      /* also .pr, .r[0..15], .sr */
```
The faulting address is in the SH-4 **TEA** register at `0xff00000c` — KOS
already reads it (`kernel/arch/dreamcast/kernel/mmu.c:27`,
`static volatile uint32 * const tea = (uint32 *)(0xff00000c);`). KOS's default
behaviour is `dbglog(DBG_DEAD, "Unhandled exception: PC %08lx, code %d, evt
%04x")` then `arch_panic("unhandled IRQ/Exception")` — so an unhandled
misalignment is loud, not silent. Good.

**Day-one deliverable for `dc/`:** an address-error handler that logs
`PC`, `PR`, `TEA`, and `r0..r15`, then either resumes past the instruction or
panics. Feed `PC` to `sh-elf-addr2line -e OpenCrossing.elf` → file:line →
the offending TU, with **no bisection needed**. This is strictly better than
the base repo's SIGBUS handler because SH-4 gives you the faulting address.

UNVERIFIED: whether **Flycast** faithfully raises the SH-4 address error for
misaligned loads, or quietly services them like a host CPU. Flycast's issue
tracker shows plenty of "Unhandled SH4 exception" reports but nothing
confirming *alignment* fidelity specifically. **Assume Flycast is permissive
until proven otherwise** — that would make Flycast unable to catch this entire
bug class, and would make real-hardware CD-R testing mandatory earlier than
PLAN §8 assumes. **Resolve this at M0** with a deliberate `*(u32*)(buf+1)`
test in the hello-world.

### 4.3 Ranked hazard list

Ranked by (execution frequency) × (probability the address is not naturally
aligned). All line numbers verified in this tree.

| # | Site | Frequency | Risk | Note |
|---|---|---|---|---|
| 1 | `src/static/jaudio_NES/internal/rspsim.c:635` — `sp120 = (s16*)cmdLo;` | per audio frame | **HIGH** | `cmdLo` is a raw address decoded from the RSP command word, pointing into ADPCM/wave-bank data at arbitrary byte offsets. Nothing forces even alignment. |
| 2 | `rspsim.c:62,120,244,245,418,419,432,471-477,606,638` — `(s16*)&DMEM[ofs]` | per audio frame, hundreds/frame | **HIGH** | `DMEM` itself is `ATTRIBUTE_ALIGN(32)` (`rspsim.c:8`) ✅, but `ofs` is `cmdLo & 0xFFFF` / `cmdLo >> 16` — command-supplied. N64 microcode convention keeps these 8/16-byte aligned; nothing *enforces* it. |
| 3 | `src/static/libforest/emu64/emu64.c:906,908,916` — `*(u32*)(converted_addr+n) = *(u32*)(addr+ofs);` | every texture upload (inner loop) | **HIGH** | `ofs = this->tmem_swap(x_ofs, line_siz)`; the 4-bit path at :916 computes `x_ofs = (blk_x + y_ofs*wd)/2`, which is **not** guaranteed ≡0 mod 4 for odd `wd`. Note the sibling 16-bit path at :848-863 was *already* rewritten byte-wise under `#ifdef TARGET_PC` — the u32 paths were not. |
| 4 | `emu64.c:890,891,898` — `u32* src = (u32*)(addr + ofs);` (IA8 path) | every IA8 texture upload | **HIGH** | same mechanism as #3. |
| 5 | `emu64.c:4715` — `emu_vtx_p->color.raw = *(u32*)(&vtx_p->v.cn[0]);` | **per vertex** | MED-HIGH | `offsetof(Vtx_t,cn)`=12, `sizeof(Vtx)`=16 (VERIFIED) → safe iff the `Vtx*` base is 4-aligned. Bases come from N64 segment-resolved addresses. Highest execution count in the whole list; a 1-in-10⁶ misalignment is still a crash every few seconds. |
| 6 | `emu64.c:3850,3879,3907,3919,3938` — `*(u16*)tlut_addr`, `(u16*)addr` | per TLUT load | MED | 16-bit access; needs only even alignment, but TLUT addresses are segment-derived. |
| 7 | `src/static/jaudio_NES/internal/system.c:30,36,46,1063,1094` — `swap32_inplace((u32*)p)`, `BANK_ENTRY` macro `(((u32*)((u32)ctrl)) + idx)` | per audio-bank load | MED | The base port's **added** LE-conversion code, walking `u8*` bank data with `u32*` casts. On DC these run over disc-streamed bank images whose in-file offsets are not our choice. |
| 8 | `system.c:983-985,1437-1439,2056` — `((u16*)rom_addr)[0..2]`, `*(u32*)wavetable` | per bank load | MED | same class. |
| 9 | `src/static/libc64/__osMalloc.c:143` — `u32* e = (u32*)(OS_MALLOC_BLOCK2DATA(block) + block->size);` | every guarded alloc/free | MED | `block->size` is a caller-supplied byte count with **no rounding**; `+ size` lands at arbitrary alignment. `:142` (base only) is fine. |
| 10 | `src/game/m_player_lib.c:1132,1201,1222,1253,1363,1368` — `(u16*)obj_ex->banks[n].ram_start`, `bcopy((u16*)pal_rom_p, …)`, `mPlib_ByteSwapPlayerPalette((u16*)pal_p)` | per player-model / palette load | MED | palette byte-swapping over ROM-sourced pointers. |

Second tier (lower frequency or lower misalignment probability, still worth
auditing):

* `src/static/JSystem/JKernel/JKRDecomp.cpp:206,214` — `*(u32*)(src_buffer+4)`,
  `*(u32*)src_buffer` (Yaz0 header; base is normally 32-aligned).
* `src/static/JSystem/JFramework/JFWSystem.cpp:38,46,47` —
  `*(u32*)(data+0x0C)`, `*(u32*)(block+0x00/0x04)` (bootdata block walk).
* `src/static/JSystem/JKernel/JKRHeap.cpp:69` — `*(u32*)((start + 0x28))`.
* `src/game/m_home.c:242,243` — `*((u32*)&home->floors[…].layer_main.ftr_switch + 1) = 0;`
  (source comment: *"ftr_switch might be a union?"* — it is a punned write).
* `src/game/m_home.c:245,251,435-438,591` — `(u16*)…layer_main.items`.
* `src/static/jaudio_NES/internal/cmdstack.c:57,78,82,124,141,143` —
  `((int*)p)[…]`, `((int (*)(int)) * (int*)(p + 0x14))(…)` — a **function
  pointer loaded from a raw byte offset**; misalignment here jumps to garbage.
* `src/static/jaudio_NES/game/melody.c:646,678` — `((u16*)AG.groups[0].seq_data)[2]`,
  `(u16*)(dst + 4)`.
* `src/game/m_camera2.c:2537` — `*(u16*)(kk_save_area + 2)`.
* `src/game/m_island.c:173,193,239,248,257` — `mISL_int(u32*,u32*)`,
  `mISL_u64(u64*,u64*)` over save/GBA transfer structs (u64 variant is two
  `mov.l` on SH-4, so 4-alignment suffices).
* `src/effect/ef_sandsplash.c:54,55,62,63,71,72` — `*(s16*)ct_arg` where
  `ct_arg` is a generic `void*` actor argument.
* `src/actor/ac_snowman.c:83,92,157,182` — `(f32*)mEv_get_common_area(...)`
  — float read out of a byte-addressed event-common area.
* `src/static/boot.c:618,621` — `(u32*)var1`, `(u32*)var2->stackEnd + 1`.

**Assessed SAFE after inspection** (listed so nobody re-audits them):
`src/system/sys_matrix.c:534,634,635` (`(u16*)&Mtx.m[0][0]` — `Mtx` is 4-aligned,
`sizeof`=64 VERIFIED, all accesses at even offsets);
`src/static/libc64/qrand.c:19,25` (`*(float*)&__qrand_itemp` — a static `u32`,
always aligned; pure aliasing, handled by `-fno-strict-aliasing`);
all `emu64.c` `*(u64*)&this->gfx` sites (1496, 3949-4001, 5122, 5349-5350,
5853) — two `mov.l` at a 4-aligned member, VERIFIED codegen.

### 4.4 The fix idiom

Do **not** sprinkle `memcpy`. Add one header, `dc/include/dc_unaligned.h`:

```c
typedef __attribute__((aligned(1))) unsigned int  u32_ua;
typedef __attribute__((aligned(1))) unsigned short u16_ua;
#define LD32U(p)     (*(const u32_ua *)(p))
#define ST32U(p, v)  (*(u32_ua *)(p) = (v))
```
VERIFIED: `aligned(1)` makes GCC SH emit the byte-wise sequence, no library
call, no alignment fault. Cost is ~6 instructions vs 1. Apply **only** at sites
the exception handler actually reports — do not pre-emptively pessimise #5
(per-vertex) without measuring.

---

## 8. Triage procedure for a miscompile

You are not bisecting 4765 files. You are bisecting at most 3900, and in the
common case you are not bisecting at all.

### Step 0 — before anything, apply the two known upstream fixes
1. Add `OSFontHeader* JUTRomFont::spFontHeader_;` to
   `src/static/JSystem/JUtility/JUTFont.cpp` (upstream `4f428276`). Without
   this, `-O2` is *expected* to produce a wild-pointer crash and you will
   waste a week bisecting a one-line bug.
2. Add `#include <string.h>` to `include/JSystem/JUtility/JUTFont.h`.

### Step 1 — make faults self-identifying (cost: ~1 hour, saves days)
Install the address-error handler at boot, before anything else:

```c
static void addr_err(irq_t src, irq_context_t *ctx) {
    volatile uint32 *tea = (uint32 *)0xff00000c;
    dbglog(DBG_DEAD, "ALIGN %s pc=%08lx pr=%08lx tea=%08lx\n",
           src == EXC_DATA_ADDRESS_WRITE ? "ST" : "LD",
           CONTEXT_PC(*ctx), ctx->pr, *tea);
    for (int i = 0; i < 16; i++) dbglog(DBG_DEAD, " r%d=%08lx", i, ctx->r[i]);
    arch_panic("alignment");
}
irq_set_handler(EXC_DATA_ADDRESS_READ,  addr_err);
irq_set_handler(EXC_DATA_ADDRESS_WRITE, addr_err);
```
Then `sh-elf-addr2line -f -e OpenCrossing.elf <pc>` → function + file:line.
**Build with `-g` always** (KOS does; `-g` costs nothing at runtime and the
ELF never ships on the disc — only `1ST_READ.BIN` does). This turns the entire
§4.3 hazard list from "audit 30 sites" into "wait for the report".

Also validate the handler at M0 with a deliberate `*(u32*)(buf+1)` — and use
that same test to answer the open question of whether **Flycast** traps it at
all (§4.2). If Flycast does not, alignment triage must happen on hardware.

### Step 2 — classify the failure
* **Address-error panic** → Step 1 named the site. Fix with §4.4's `LD32U`
  idiom. No bisection.
* **Fault at a plausible-but-garbage PC / wild pointer** → suspect a missing
  definition or an inlined-away `#pragma dont_inline`. Diff the `-O0` and
  `-O2` symbol tables (`sh-elf-nm -u`) for symbols that appear only at `-O2`;
  that is exactly how `spFontHeader_` presents.
* **Silent wrong behaviour** → Step 3.

### Step 3 — bisect the *flag set*, not the files, first
Cheaper than any file bisection because each step is one full build:
`-O2` → `-O1` → `-O2 -fno-tree-vectorize` → `-O2 -fno-ipa-sra` →
`-O2 -fno-strict-aliasing -fno-schedule-insns2` → `-Os`.
6 builds ≈ 45 min total (measured: 650 TUs ≈ 67 s at `-P4` under colima, so a
full 3900-TU build ≈ 7 min).

### Step 4 — bisect the *set of TUs at -O2*, by directory
Drive it from a plain text file of TU paths with a per-TU `-O` override, so
one variable controls the partition. Bisect over these ~20 groups first, in
descending suspicion order:

```
src/static/libforest/emu64/   (6.4k LOC — already proven -O2-safe on ARM)
src/static/jaudio_NES/        (61 files, 33k LOC — rspsim is hazard #1/#2)
src/static/JSystem/           (heaps, archives, Yaz0)
src/game/                     src/actor/       src/effect/
src/system/                   src/bg_item/     src/static/libc64/
src/static/libjsys/           src/static/libu64/   src/data/  (inert tables)
src/*.c (top level)
```
`log2(20) ≈ 5` builds ≈ 35 min to pin the directory. Then binary-search within
it: `log2(600) ≈ 10` builds ≈ 70 min. **Worst case ≈ 2 hours from "it's broken
at -O2" to "this TU".** Compare: naive per-file bisection over 3900 is
`log2(3900) ≈ 12` builds — barely different, so just do the directory version,
it produces a more useful intermediate answer.

### Step 5 — per-TU fallback, permanently
Mirror the base repo's mechanism exactly: a list of TUs pinned to a lower `-O`,
with a comment naming the symptom and the date. The base repo's inverse
(`emu64.c` pinned *up* to `-O2`, with a DEVICE-TEST GATE comment) is the same
pattern and should be preserved in spirit: **any change to a TU's optimization
level requires a full new-game intro run** (KK Slider → train → town arrival),
because that is the sequence that historically exposed the alignment class.

### Step 6 — feed fixes upstream
Every misaligned cast fixed at the source is upstreamable to
`flyngmt/ACGC-PC-Port` and benefits every port. The `#ifdef TARGET_PC`
byte-wise rewrite already applied to `emu64.c:840-864` is the model: keep the
original expression under `#else` so the decomp stays matching.

---
