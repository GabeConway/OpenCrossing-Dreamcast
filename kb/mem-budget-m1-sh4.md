# RAM budget — M1 post-link measurements on the real sh-elf ELF (§8)

The first numbers taken from an actual Dreamcast link (`sh-elf-gcc 15.2.0`,
`-O0`, 3,917 TUs): section sizes, the `.bss` split, the levers applied and the
dead ends, why `--gc-sections` is mandatory, the optimization table that policy
rejected (**and 2026-08-06 adopted — see the banner**), and the boot attempt
that proved the silence is image size alone.
**This is the part of the old ledger that is still true**; it supersedes the
armhf extrapolations of the old ledger (deleted 2026-08-09) wherever they
disagree. Live running numbers are in `kb/STATE.md`.

> ## ⚠️ [STALE 2026-08-06] — §8.6's "REJECTED BY POLICY" is now "ADOPTED"
>
> **The `-O0` directive was reversed on 2026-08-06.** `src/` builds at `-Os`
> with an 18-TU `-O3` hot list (`DC_OPT_PROFILE=perf`, the default; `size` =
> `-Os` everywhere; `o0` = byte-identical revert); `dc/src` moved from `-O2` to
> `-O3`. Measured on matched town windows of the shipping build:
>
> | | `-O0` | `-Os` | shipping (`-Os` + `-O3` hot) |
> |---|---:|---:|---:|
> | `.text` | 5,506,964 | 2,680,676 | 2,753,700 |
> | `.data` | 2,337,980 | 2,224,832 | 2,224,832 |
> | `.bss` | 3,945,356 | 3,945,484 | 3,945,484 |
>
> `.text` fell 2,826,288 B at flat `-Os`; `.data` fell 113,148 B; `.bss` did not
> move (+128 B). **The codegen lever was worth roughly every `.bss` lever this
> project has landed put together.**
>
> What that voids in this document: **§8.6's rejection**, and **§8.8's "the
> remaining 13.34 MB cannot come from codegen (policy)"** — it can, and 2.75 MB
> of it did. §8.1's `.text`/`.data` rows, and every total derived from them, are
> `-O0`-era. What survives untouched: §8.2 (the `.bss` split), §8.3 (the four
> `.bss` levers), §8.4 (strip = 0, NES ≈ 40 KB), §8.5 (`--gc-sections` is
> mandatory and already spent), §8.7 (boot is gated on image size).
>
> ⚠️ The table above is from the *shipping stubbed town* build line, not the
> full-asset link §8 measures — do not substitute one into the other. Evidence:
> the 2026-08-06 entry of `kb/state-log.md`.

## 8. M1 post-link measurement — the real numbers from a real Dreamcast ELF

**Measured 2026-08-01 on `dc/build/AnimalCrossing.elf`**, the first ELF this
project has ever linked for SH-4. Everything in §§1–7 above was measured on the
*armhf* build and extrapolated; this section supersedes those extrapolations
wherever they disagree. All figures **[M]** unless marked.

Subject: `sh-elf-gcc 15.2.0`, KOS master, `DECOMP_OPT=-O0`, 3,917 TUs.
Tools: `sh-elf-size -A`, `sh-elf-nm --size-sort -S`, `sh-elf-readelf -lW`,
`sh-elf-objdump -h`, all inside `opencrossing-dc:sdk`.

### 8.1 Section sizes, before and after this pass

| | `.text` | `.data` | `.bss` | allocated total | image end |
|---|---:|---:|---:|---:|---|
| M1 first link | 6,318,568 | 2,638,852 | 13,526,548 | **22,486,548** | `0x8d581c14` |
| after §8.3 fixes | 6,318,568 | 2,638,852 | **12,415,508** | **21,374,996** | `0x8d472814` |

`_arch_mem_top` (stock 16 MB console) is `0x8d000000`. The image alone ends
**4,663,316 B past the top of RAM**, before a single byte of heap.

> **Correction to §4, bucket 6.** `DC_MAIN_MEMORY_SIZE` (4,000,000 B) is **not**
> in `.bss`. `dc/src/dc_os.c:400` obtains it with `dc_mem_alloc(DCMEM_JKRHEAP,…)`
> at runtime, so it sits on the KOS heap **on top of** the 21.37 MB image. Any
> ledger that counted the arena inside `.bss` understates the overage by 4 MB.

### 8.2 The `.bss` split that decides the next lever

`.bss` symbol-attributed total 13,603,920 B across 17,776 symbols (slightly
above the section figure — per-object sums include alignment padding).

Destination symbols were recovered from source, read-only: the 14,495-entry
`s_assets[]` table in `pc/src/pc_assets.c` plus 1,848 `pc_load_asset(...)` call
sites in the generated `_pc_load_src_*()` functions under `src/` — **15,704
unique destination symbols**.

| | bytes | symbols | share |
|---|---:|---:|---:|
| **asset-destination arrays** | **8,771,358** | 16,340 | **64.5 %** |
| everything else | 4,832,562 | 1,436 | 35.5 % |

**8,771,358 B matches §2's independently-derived "Total loaded at boot into
BSS" to the byte**, and matches the asset-pack agent's ~8,617,214 B from a
third direction. Triple-corroborated.

By owning tree:

| bucket | total | asset-dest | non-asset |
|---|---:|---:|---:|
| `src/data` | 8,519,191 | 8,518,368 | 823 |
| `src/game` | 1,548,236 | 2,448 | 1,545,788 |
| `src/static/jaudio_NES` | 1,265,101 | 0 | 1,265,101 |
| `src/actor` | 711,058 | 138,048 | 573,010 |
| `src/static/libforest` (emu64) | 562,374 | 0 | 562,374 |
| `dc/src` | 372,585 | 0 | 372,585 |
| `pc/src` (reused) | 320,158 | 0 | 320,158 |
| `src/other` | 173,485 | 1,280 | 172,205 |
| `src/static/other` | 113,466 | 111,214 | 2,252 |
| `src/static/JSystem` | 18,266 | 0 | 18,266 |

**Conclusion: the next lever is demand-loading into pooled storage, not arena
right-sizing.** Two thirds of `.bss` is asset destinations; no amount of
right-sizing the remaining third can close a 13 MB gap.

Top 10 **non**-asset-destination `.bss` symbols (the genuinely-classifiable
part), each tagged GC-sized / N64-era / required:

| bytes | symbol | object | class |
|---:|---|---|---|
| 1,228,800 → 614,400 | `prbuf` | `src/game/m_play.c` | GC-sized, halved in §8.3 |
| 589,824 | `audiomemory` | `jaudio_NES/game/game64.c` | GC-sized (→ AICA) |
| 524,288 → 49,152 | `texture_buffer_data` | `emu64/emu64.c` | PC-inflated, reverted in §8.3 |
| 334,088 | `g_gx` | `dc/src/dc_gx.c` | ours, already shrunk from PC's 3.15 MB |
| 294,912 | `aSTR_overlay` | `src/actor/ac_structure.c` | GC-sized (32 × 0x2400) |
| 275,456 | `seq` | `jaudio_NES/internal/seqsetup.c` | required (sequence data) |
| 187,328 | `common_data` | `src/game/m_common_data.c` | **required** (live save state) |
| 155,648 | `l_keepSave` | `pc/src/pc_m_card.c` | GC-sized (456 KB GC save) |
| 132,104 | `sys_dynamic` | `src/system/sys_dynamic.c` | required (GX DL build buffers) |
| 81,920 | `CHANNEL` | `jaudio_NES/internal/driverinterface.c` | required |

**There is no emulated-N64-RDRAM buffer.** The N64-origin lead was checked
directly against the linked image: the entire `src/static/libforest` (emu64)
tree is 562,374 B of `.bss`, the largest single item being
`texture_buffer_data`. No 4 MB or 8 MB machine-memory image exists anywhere in
the binary. This agrees with `kb/closed.md`: emu64 is a GBI
display-list interpreter, not a machine emulator.

### 8.3 Levers applied this pass (measured individually)

Sum of the four = 1,111,040 B, and the measured `.bss` delta is exactly
1,111,040 B — nothing else moved.

| lever | Δ `.bss` | where | why it is safe |
|---|---:|---|---|
| `prbuf` `sizeof(u32)`→`sizeof(u16)` | −614,400 | `src/game/m_play.c:54`, `#ifdef TARGET_DC` | the only writer is `copy_efb_to_texture()` → `GXSetTexCopyDst(…, GX_TF_RGB565, 0)` = 2 B/px; the only reader binds `G_IM_SIZ_16b`. VERIFIED at both ends in this tree. The 2× is dead on GameCube too. |
| `TEX_BUFFER_DATA_SIZE` `0x80000`→`0xC000` | −475,136 | `include/libforest/emu64/texture_cache.h:71` | reverts a PC-port inflation to the **retail GameCube** value; sufficiency proven by the shipped product |
| `TEX_BUFFER_BSS_SIZE` `0x4000`→`0x400` | −15,360 | same file | same |
| `TEXTURE_CACHE_LIST_SIZE` `1024`→`256` | −6,144 | same file:16 | same |

All four are `#if defined(TARGET_DC)` branches added alongside the existing
`TARGET_PC` guards — the PC and GameCube paths are byte-for-byte untouched.

### 8.4 Levers measured and found to be dead ends

| lever | measured result |
|---|---|
| `-g0` / strip debug info | **0 B of RAM.** `sh-elf-readelf -SW` shows every `.debug_*`, `.symtab`, `.strtab` section has no `A` flag — they are not in any `PT_LOAD`. Saves ELF/disc bytes only. |
| drop NES/famicom emulation | **≈39,444 B `.bss` + 1,724 B `.text`.** `src/static/Famicom/` is already excluded by the build filters; what remains in-image is `famicom_emu.c` (20 B), `int_tak_nes01` (14,208 B) and six `FONT_nes_*` textures (25,216 B). The 1.70 MB `famicom.arc` is **disc** space, not RAM. Not worth the risk. |
| drop debug / symbolication machinery | **≈88 KB total** (37,938 `.bss` + 42,392 `.text` + 8,337 `.data`/`.rodata`), of which `dvderr.c` is 33,204 B of the `.bss`. Marginal. |
| `src/static/jaudio_NES` as a "non-goal" | **Not a non-goal.** Despite the name it is the *game audio engine* (1,265,101 `.bss` + 319,194 `.text`), not the NES emulator. Dropping it removes all sound. |

### 8.5 `--gc-sections` is mandatory, not an optimization

`DC_GC_SECTIONS=0` **does not link at all**. Without section GC the decomp has
genuinely undefined symbols whose referencing sections are unreachable —
`JKRTask::searchBlank()`, `vtable for JSUOutputStream`, `vtable for
JSURandomOutputStream`, `JSURandomOutputStream::getAvailable() const`,
`::skip(long)` — plus KOS's own `__kos_romdisk`. So "with vs without" cannot be
compared on two linked images; the saving has to be read off the discard list.

`-Wl,--print-gc-sections` reports **16,164 dropped sections**; summing their
sizes from each object's section headers gives **395,207 B**
(`.text` 87,744 · `.rodata` 49,043 · `.data` 6,062 · `.bss` 110,674 · other
141,684). `kb/levers.md` reports 522,150 B over 29,471
sections from the link map, which also counts library objects this per-object
sweep did not reach — take the map figure as authoritative. Either way it is
already spent, and it is ~0.4–0.5 MB, not megabytes.

Codegen-identity note: `-ffunction-sections -fdata-sections` change only which
named section a function is emitted into, never instruction selection. This is
therefore **not** an optimization and is compatible with the `-O0` policy.

### 8.6 Optimization levels — measured, then ~~REJECTED BY POLICY~~ **ADOPTED [2026-08-06]**

Recorded once so the number is not mistaken for unknown. ~~**User decision
2026-08-01: decomp game code stays at `-O0`.** Do not propose these.~~

⚠️ **[2026-08-06] the 2026-08-01 decision was REVERSED.** `src/` ships at `-Os`
with an 18-TU `-O3` hot list. The four-row table below is a full-asset `-O0`-era
link and is kept as history; the shipping numbers are in the banner at the top
of this file (`.text` 5,506,964 → 2,753,700 B on the town build line). **This
section was right that the numbers existed and wrong only about what to do with
them.** `kb/state-log.md`, 2026-08-06.

| `DECOMP_OPT` | `.text` | `.data` | `.bss` | total |
|---|---:|---:|---:|---:|
| `-O0` (policy) | 6,318,568 | 2,638,852 | 13,526,548 | 22,483,968 |
| `-O1` | 3,697,460 | 2,525,704 | 13,526,164 | 19,749,328 |
| `-Os` | 3,477,188 | 2,525,704 | 13,526,132 | 19,529,024 |
| `-O2` | 3,825,312 | 2,525,672 | 13,526,132 | 19,877,116 |

The load-bearing observation is that **even `-Os` does not reach 16,777,216 B**,
let alone the 8,035,072 B image budget. Optimization was never sufficient on
its own, so the policy costs less than it appears to: the budget always had to
come out of `.bss`.

⚠️ **[2026-08-06] "sufficient on its own" was the wrong test, and it is what
made this paragraph misleading for five days.** Nothing is sufficient on its own
here; the question is what each lever is worth. Optimization was worth
2,826,288 B of `.text` — measured, at flat `-Os`, on the shipping town build —
which is the same order as the entire `.bss` programme. "It does not close the
gap by itself" was true of every lever in this document.

The one real consequence: §4's bucket 3 budgets `.text` at 2,600,000 B, which
is only reachable with optimization. At `-O0` it is 6,318,568 B — **3.7 MB over
bucket 3, against a ledger whose entire margin was 1.75 MB.** §4 no longer
closes as written, independently of the asset work.
⚠️ **[2026-08-06]** and optimization is now on, so bucket 3 is no longer 3.7 MB
out of reach: the shipping `.text` is **2,753,700 B** against that 2,600,000 B
target — the same neighbourhood, not a different order of magnitude. (Different
build lines; do not subtract them and call it margin.)

### 8.7 Boot attempt — mechanism confirmed, no crash to symbolise

`harness/dc/smoke.sh dc/build/OpenCrossing.cdi --timeout 90` → **exit 1,
`timeout`, zero bytes of console output.** No KOS banner, no maple
enumeration, nothing.

Controls run to attribute it:

| image | `.bss` | image end | result |
|---|---:|---|---|
| `selftest.cdi` (harness reference) | 22,728 | `0x8c048948` | **PASS, 3.10 s** |
| hello-world + 4.7 MB `.bss` | 4,722,728 | `0x8c4c40a8` (under top) | **PASS, 3.08 s** |
| hello-world + 21 MB `.bss` | 21,022,728 | `0x8d44f888` (over top) | **FAIL, 0 bytes console** |
| `OpenCrossing.cdi` | 12,415,508 | `0x8d472814` (over top) | **FAIL, 0 bytes console** |

A stock KOS hello-world containing *nothing but* a large `.bss` array fails
identically once its image end crosses `_arch_mem_top`, at essentially the same
address as the game. So the game's silence is **fully explained by image size
alone** — it is not a game fault, not the `dc_main.c` exception trampoline, and
not something `-DDC_NO_CRASH_PROTECTION` can distinguish. The guest
never executes an instruction, so **there is no PC to symbolise**.

Mechanism: KOS has no MMU paging and `mm_sbrk()` starts at the ELF `end`
symbol, so `.bss` is committed RAM and the startup zeroing walks past the top
of physical memory before `scif_init()` ever runs.

**Nothing about the port's correctness can be tested until the image fits.**
Boot is gated on size, and only on size.

### 8.8 Where the gap stands

Against the 8,035,072 B image budget from `kb/levers.md`
(which reserves the ledger's own 7.61 MB of heap plus ~1 MB for KOS):

| | bytes |
|---|---:|
| allocated image today | 21,374,996 |
| image budget | 8,035,072 |
| **still to shed** | **13,339,924** |

This pass removed 1,111,040 B of it. ~~The remaining 13.34 MB cannot come from
codegen (policy)~~, from stripping (0 B), from `--gc-sections` (already spent),
or from dropping the NES path (~40 KB). It has to come from the ranked plan in
`kb/levers.md` — of which **8.45 MB is demand-loading the
`src/data` staged assets**, i.e. the asset pack's runtime loader. That is the
critical path, and it is the next milestone's work.

⚠️ **[2026-08-06] "cannot come from codegen (policy)" is VOID.** The policy was
reversed; codegen delivered `.text` 5,506,964 → 2,753,700 B on the shipping
town build (2,680,676 at flat `-Os`), i.e. ~2.75 MB — the second-largest single
lever in the project, behind only `src/data` demand residency and ahead of every
`.bss` lever landed to date. The 13,339,924 B figure above is `-O0` arithmetic
against a superseded budget; do not carry it forward. Demand residency is still
the critical path. Evidence: the 2026-08-06 entry of `kb/state-log.md`.
