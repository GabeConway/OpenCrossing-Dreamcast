# RAM budget — the VOID 16 MB ledger (§4) and its open questions (§7)

⚠️ **This ledger does not close and cannot be used as written.** Killed
2026-08-01 by two facts: (1) optimization was banned by user directive, so
bucket 3's 2,600,000 B `.text` target is unreachable — the real `-O0` figure is
6,318,568 B, 3.7 MB over a ledger whose entire margin was 1,750,608 B; and (2)
the first real sh-elf link showed the 4 MB arena is *not* in `.bss`, so any
ledger counting it there understates the overage by 4 MB (see
`kb/mem-budget-m1-sh4.md` §8.1). The live replacement is `kb/ram-plan.md` +
`kb/levers.md`, with current numbers in `kb/STATE.md`. Kept verbatim because
other docs cite its bucket numbering (C1–C11, buckets 1–12), and because §7's
open measurements — all except #2, which optimization policy closed — are
still open. The corrections block below is the original document's own header.

> ## ⚠️ Two corrections since this was written (2026-08-01, later same day)
>
> 1. **Every `-O2`/`-Os` remedy below is void.** Optimization is banned by
>    user directive (CLAUDE.md, PLAN §3.2) — `src/` builds at `-O0`, full
>    stop. Wherever this document proposes `-O2` for hot TUs or `-Os` for cold
>    ones (bucket 3, and the open-question list), read it as **"unresolved,
>    must be closed by a layout-class lever instead"**: `--gc-sections`,
>    `.bss` right-sizing, linker placement, moving data to `/cd`, or dropping
>    non-goal subsystems.
> 2. **The real sh-elf link now exists**, so bucket 3 no longer needs an ARM
>    proxy. Measured at `-O0`, all 3917 TUs, zero exclusions:
>    **text 6,318,568 / data 2,638,852 / bss 13,526,548 = 22,483,968 B.**
>    That is ~6.5 MB over budget before heap headroom.
>
> Also landed since: the resident REL blob (16,558,776 B) is solved by
> `dcasset pack` (−15.68 MB, see `kb/asset-pack.md`), which in turn exposed
> **8.22 MB of static asset destination arrays** — 15,726 of them, resident
> regardless of source, and now the single largest line in this ledger. It
> needs its own bucket. `kb/STATE.md` carries the live numbers.

## 4. The 16 MB ledger (task 4 answer)

Budget = 16,777,216 B. Every bucket below names the code change that hits its
target. **Sum 15,026,608 B (89.6 %), margin 1,750,608 B (10.4 %).**

| # | Bucket | Today [M] | DC target | Δ | Code change required |
|---:|---|---:|---:|---:|---|
| 1 | KOS kernel + newlib + drivers (text/data/bss/kernel structs) | n/a | 1,000,000 [?] | — | none — but **measure at M0**: a KOS hello-world with GLdc linked, `(uintptr_t)_arch_mem_top - (uintptr_t)&end` after `pvr_init`. If it lands >1.5 MB, take it from bucket 6. |
| 2 | Reserved low RAM `0x8C000000–0x8C010000` | n/a | 65,536 | — | fixed by hardware/KOS. Not spendable. |
| 3 | Game+platform `.text` | 3,327,876 | 2,600,000 [?] | −727,876 | **C1**: `-O2` for hot TUs, `-Os` for cold ones (game code is `-O0` today). **C2**: drop `glad`/`pc_gx_tev`/`pc_texture_pack`/`pc_model_viewer`/famicom glue (−90 KB measured). SH-4 vs ARM code-size ratio is **unknown** — resolve at M1, first link. |
| 4 | Game+platform `.rodata` + `.data` | 3,012,230 | 1,800,000 | −1,212,230 | **C3**: run `gen_runtime_assets.py` over the 948,688 B of measured pointer-free `.data` symbols (mechanism already exists, just not applied to non-`.inc` tables). **C4**: replace `s_assets[]` (347,880 B) with a disc-side index + ≤64 KB in-RAM hash. |
| 5 | Game+platform `.bss` (true runtime state) | 9,472,997 | 1,950,000 | −7,522,997 | itemised in §4.1 |
| 6 | JKRHeap system heap + `__osMalloc` game arena | 25,153,072 reserved, **usage unmeasured** | 4,000,000 [?] | — | **C5**: set `JFWSystem::CSetUpParam::sysHeapSize` from measured peak (§6) instead of `arena_hi - arena_lo`, and stop `jsyswrap.cpp:547` handing the whole remainder to `MallocInit` — give it a fixed, ledgered extent. **This is the single biggest unknown in the budget.** |
| 7 | Asset residency pool (was 8.77 MB of static BSS) | 8,771,358 | 1,500,000 | −7,271,358 | **C6**: convert the 2,080 asset-owning objects into ~2,080 disc-resident groups with a generated ID table; `gen_runtime_assets.py` emits pointer-indirection stubs instead of arrays; runtime LRU pool keyed by group ID, warmed per acre/room load. Kills the 15.64 MB REL transient at the same time (assets are read individually from a packed disc file). |
| 8 | Graph-ARAM (RARC) LRU window | 6,961,536 reserved / 4,985,504 real | 512,000 [?] | −6,449,536 | **C7**: `dc_aram.c` replaces `ARStartDMA`'s memcpy with a paged disc read against `forest_{1st,2nd}.arc` laid out on outer tracks. Window size is a guess — **measure at M1** (§6, probe 3). |
| 9 | Audio work RAM, SH-4 side (`audiomemory`, `seq`, `CHANNEL`, `dmabuffer`, `dvd_buf`, `CALLSTACK`, `AG`, `CH_BUF`, `pc_task_buf`, `ring_buffer`, `SoundQbuf`, `sound_loop_buffer`) | 1,297,312 | 700,000 | −597,312 | **C8**: `audiomemory` `0x90000`→`0x40000`; `snd_stream` double-buffer replaces the 65,536 B SDL ring; sequence data only in main RAM — samples live in AICA's separate 2 MB. Sound ARAM (8,458,240 B) → **0 main RAM**. |
| 10 | Disc read-ahead ring + KOS VFS buffers | ~470,000 (`dvd_entry_table` + `dvd_buf`) | 384,000 | — | **C9**: read-ahead thread with a 3 × 128 KB ring (CD-R ≈ 500 KB/s ⇒ ~0.75 s of lookahead). Sized to hide the base port's known 8.7 s stall class. |
| 11 | PVR vertex staging / TA buffers in main RAM | n/a | 384,000 | — | **C10**: stage-A GLdc holds a vertex array in main RAM; stage B submits via store queues with only a small block buffer. Framebuffers and textures cost **0 main RAM** (8 MB VRAM is a separate address space). |
| 12 | Thread stacks (main 64 K, audio 32 K, read-ahead 16 K, KOS idle/misc) | n/a | 131,072 | — | **C11**: explicit `kthread` stack sizes, no defaults. |
| | **TOTAL** | | **15,026,608** | | **margin 1,750,608 B / 10.4 %** |

Separate address spaces, budgeted elsewhere (not part of the 16 MB):
**8 MB VRAM** (framebuffers + all textures, twiddled/VQ/paletted) and
**2 MB AICA sound RAM** (8,300,384 B of `audiorom.img` does *not* fit — needs
offline bank splitting + streaming; that is PLAN §3.4's problem, and it needs
its own ledger).

### 4.1 Bucket 5 derivation — measured, not guessed

Starting from the measured 9,472,997 B of true-runtime BSS:

| Item | Bytes | Action |
|---|---:|---|
| `g_loaded_cache` + `g_neg_cache` (texture packs) | −802,816 | delete (non-goal) |
| `g_fst_files` (ISO FST) | −270,336 | delete (host-side tool) |
| `quad_index_buf` (GL) | −196,608 | delete |
| `s_cache` (GLSL shader cache) | −165,888 | delete |
| `ov_verts` (SDL overlay), net of a 12,288 B DC overlay | −86,016 | shrink |
| `glad` BSS | −4,928 | delete |
| `g_gx` 65536 × 48 B → 8192 × 32 B + state ≈ 300,000 | −2,852,684 | shrink (`PC_GX_MAX_VERTS`, `PCGXVertex`) |
| `prbuf` EFB capture → PVR render-to-texture in VRAM | −1,228,800 | move to VRAM (fallback: 320×240×2 = 153,600 in RAM) |
| `texture_buffer_data` (emu64 `0x80000` → `0x20000`, decode to VRAM) | −393,216 | shrink |
| `dvd_entry_table` 133,120 → 16,384 | −116,736 | shrink |
| `pc_m_card` keep-buffers 303,430 → 160,000 (compressed VMU staging) | −143,430 | shrink |
| misc `pc_disc`/`pc_dvd` residue | −30,000 | delete |
| audio BSS reclassified into bucket 9 | −1,297,312 | move |
| **remaining** | **1,871,939** | |

Target 1,950,000 B leaves 78 KB of slack inside the bucket. Everything left
is genuinely live state: `common_data` (187,392), `sys_dynamic` (132,112),
`aSTR_overlay` (294,912), the `a*_overlay`/`a*_prg`/`a*_ldr` actor overlay
buffers, `tex_cache` (98,304), `JUTResFONT_Ascfont_fix12` (16,736), and a
~1.35 MB tail of ~4,000 small symbols.

### 4.2 Verdict

**16 MB is reachable, with ~10 % margin, but only if bucket 6 lands at or
under 4 MB — and that number is currently a guess.** Everything else in the
ledger is either measured or bounded by a mechanical code change.

**The single biggest lever is C6, the asset residency pool: 7.27 MB.** It is
also the change that removes the 15.64 MB boot transient, so it is both the
largest and the most urgent. Second is the arena (C5, unknown but nominally
21 MB of reservation), third is graph-ARAM windowing (C7, 6.45 MB).

Risks to the margin, honestly stated:

- Bucket 3 (`.text` 2.6 MB) is a **[?]**. If SH-4 `-O2` output exceeds ARM
  `-O0` output, the whole 1.75 MB margin can evaporate on this one line.
  It is the first thing M1 must report.
- Bucket 1 (KOS 1.0 MB) is quoted from `kb/research-dreamcast.md`'s "~2–4 MB
  practical", which we have **not verified** and which we believe is
  pessimistic because it likely folds in GLdc's own buffers (our bucket 11).
  If KOS really costs 2.5 MB, the budget is at 16.5 MB and something must go.
- Fragmentation is not modelled. Bucket 6 is a first-fit expheap plus a
  second-level `__osMalloc`; long-session fragmentation could force a larger
  reservation than the measured peak. Add ≥15 % on top of the measured peak
  when setting `sysHeapSize`.
- The "DC edition" content cuts (famicom.arc 1.7 MB, museum exhibits, island)
  buy `.text` and disc space but almost no RAM — do not count on them.

---

## 7. Open measurements (ordered by how much of the budget they decide)

| # | Question | Decides | Milestone |
|---|---|---|---|
| 1 | Real `__osMalloc` + `JKRExpHeap` high-water over a full playthrough | bucket 6 (4.0 MB, 24 % of RAM) | M1, §6 probe 1+2 |
| 2 | SH-4 `-O2`/`-Os` code size vs ARM `-O0` | bucket 3 (2.6 MB, 15 %) | M1, first link |
| 3 | Graph-ARAM 5 s working set | bucket 8 (512 KB) | M1, §6 probe 3 |
| 4 | Distinct asset groups live per acre/room | bucket 7 (1.5 MB) | M1, §6 probe 4 |
| 5 | KOS + GLdc real overhead on a hello-world | bucket 1 (1.0 MB) | M0 |
| 6 | Peak `g_gx.vertex_count` in a town frame | bucket 5 (1.95 MB) | M1, §6 probe 4 |
| 7 | `__osMalloc` fragmentation over a long session | the margin on bucket 6 | M3 |

Until #1 and #2 report, treat the 10.4 % margin as provisional.

---
