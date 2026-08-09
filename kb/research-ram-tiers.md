# RAM: the second-tier ideas, and two corrections to the existing arithmetic

Written 2026-08-04, from a deliberately out-of-the-box pass over the 16 MB
problem, then vetted against `kb/closed.md` idea by idea. The companion to
`kb/levers.md` (which this does not repeat) and to
`kb/levers.md` (whose arithmetic it corrects).

**Every idea below was cross-checked against `kb/closed.md`. None reopens MMU
paging, AICA-as-C-arrays, `--icf`, strip/compress, or ~~`-O1+`~~.**

> ## ⚠️ [2026-08-06] `-O1+` WAS REOPENED, AND IT WAS WORTH MORE THAN THIS WHOLE PAGE
>
> The `-O0` directive was reversed. `src/` builds at `-Os` with an 18-TU `-O3`
> hot list; `dc/src` moved `-O2` → `-O3`. Measured, matched town windows:
>
> | | `-O0` | `-Os` | `-Os` + `-O3` hot | + `dc/src` `-O3` |
> |---|---:|---:|---:|---:|
> | `.text` | 5,506,964 | 2,680,676 | 2,729,152 | **2,753,700** |
> | `.data` | 2,337,980 | 2,224,832 | 2,224,832 | **2,224,832** |
> | `.bss` | 3,945,356 | 3,945,484 | 3,945,484 | **3,945,484** |
>
> **`.text` −2,753,264 B in one flag.** That is larger than R1+R3+R4+R6
> combined, at none of their hardware risk. Evidence: `kb/state-log.md`, top
> entry, 2026-08-06.
>
> **What it does to this page:**
>
> - **R6 loses its premise outright.** R6 opens "`-O0` means `.text` can only
>   *move*". `.text` did not have to move; it had to be compiled. See R6.
> - **R1-R5 and R7-R8 are unaffected in mechanism** — they attack `.bss`, the
>   arena and libc heap, none of which moved (`.bss` +128 B). They ARE affected
>   in urgency: the gap they were sized against is 2.75 MB narrower, so the
>   hardware-gated ones (R1/R3/R4/R5, all waiting on a `bench_mem` burn) should
>   be re-justified rather than assumed necessary.
> - **R9's first bullet ("drop jaudio") is now doubly uncertain**: its FPS half
>   is an `-O0` measurement, and the software-vs-AICA audio verdict it leans on
>   has reopened (`kb/audio-plan.md`).

---

## Two corrections. These are worth more than several of the ideas.

### C-1. `kb/levers.md` P4 is stale by ~917 KB

P4 ("ARAM graph window → VRAM") is billed at **−1,048,576 B of additive heap**.
But the disc-backed pager that landed 2026-08-02 already shrank the ARAM line to
**131,072 B** (`kb/STATE.md`: additive heap 2,358,752 = KOS 262,144 + arena
1,900,000 + ARAM LRU 131,072 + threads 65,536).

**The realisable value of P4 today is −131,072, not −1,048,576.** The ram-plan
closing table still credits the old figure, so anyone re-running that arithmetic
starts ~0.9 MB optimistic. This is exactly the class of error the
one-inequality rule exists to catch.

### C-2. `kb/texture-path.md` and `kb/levers.md` P2 contradict each other on VQ

`texture-path.md` §2 says "VQ is closed too" — KOS's `PVR_TXRLOAD_VQ_LOAD` is
unsupported and SH-4 codebook generation is infeasible. `ram-plan.md` P2
proposes offline VQ via `dcasset`, citing the dca3 precedent.

They are reconcilable: **the closure is about RUNTIME encoding.** Offline
conversion is an explicitly legal layout lever, and uploading a pre-built VQ
texture needs only a codebook+index copy plus the VQ bit in the poly header —
not `PVR_TXRLOAD_VQ_LOAD`. But as written, the two documents will make a future
session drop P2 citing texture-path. One sentence of reconciliation is owed to
whichever doc the main thread prefers.

---

## The ideas, ranked by (bytes recovered) / (risk x effort)

### R1. ARAM LRU block cache → VRAM. −131,072 B, one seam.

`dc/src/dc_aram.c:664` allocates the 4 x 32 KB LRU cache with a single
`dc_mem_alloc(DCMEM_ARAM_GRAPH, …)`. Replace with `pvr_mem_malloc(131072)`;
hits become 32-bit-aligned VRAM reads, fills become `fs_read` into a bounce.

Why it is safe here: all access is bulk copy — the shape `kb/closed.md` says
second-tier memory remains legal for — and the pager already treats the cache as
expendable, since it can always re-read from `/cd`. That also means VRAM
eviction pressure can shrink it without correctness risk.

⚠️ Failure mode: **SH-4-from-VRAM read bandwidth is the project's one admitted
missing number.** Reads here are 64 B to 32 KB at scene/dialogue granularity, so
even 10 MB/s is sub-millisecond per fetch — but that must be measured.

This is the surviving kernel of P4 after correction C-1.

### R2. `-Wl,--wrap=malloc` — make the two pools actually share. ~0.5-1 MB of margin un-reserved.

The link already carries custom flags (`dc/Makefile:853`). Add
`-Wl,--wrap=malloc` (and `memalign`): `__wrap_malloc` calls `__real_malloc`, and
on NULL triggers the S4 pool's eviction callback, then retries once. ~40 lines
in `dc/src/`, no `src/` edit, kill switch = drop the flag.

**Why this specifically.** `kb/heap-two-pools.md`'s whole pathology is that the
sbrk OOM is fatal while reclaimable bytes sit idle elsewhere. P3 shares slack
between the *arena* and the pool; nothing currently shares slack with **libc** —
the pool that actually OOMed in practice. This closes the triangle. It converts
the "bucket 6 truth: +0.8-2.8 MB" margin claimant in `ram-plan` §1 from a
reserved worst case into pay-on-demand.

⚠️ Failure modes: re-entrancy (an eviction that itself allocates), and an
eviction storm hiding a real leak so the OOM arrives later and stranger. Detector
is an eviction counter on the `MEMLEDGER` line plus a loud debug mode.

It moves neither side of the inequality — it makes the additive-heap line honest
at a smaller number.

### R3. The ARAM save blocks → VRAM. −147,840 B, permanent.

`kb/levers.md` L8: `mCD_save_data_aram_malloc` (`src/first_game.c:24`) takes
**147,840 B** of permanent libc heap for three ARAM save blocks whose access
pattern is bulk copy on save/load. A `DCMEM_VRAM_BULK` class backed by
`pvr_mem_malloc` removes them from main RAM rather than moving them between
competing pools. `pin_peak=0` in the pager logs confirms nothing touches them
per frame, and the VMU path is seconds-scale anyway (84.6 ms per VMU block
dwarfs any VRAM copy).

⚠️ Before moving: audit every reader for scattered byte access — VRAM dislikes
8-bit access. And "does a non-texture blob survive untouched in VRAM across PVR
scene operations" is *unestablished*; a checksum-across-frames probe answers it.

### R4. VRAM spare as the S4 pool's eviction tier. ~0.5-1 MB off the pool ceiling.

P3 makes pool contents evictable by construction, but today "evict" means
discard and re-read from CD at 500 KB/s with seeks. Add a middle tier: evicted
blocks are copied into `pvr_mem_malloc` space (texture-path §5 measured ~3.5 MB
of headroom below the soft ceiling on the town run) and restored at 100+ MB/s.

**Why it matters more than its own byte count:** the pool ceiling is the binding
constraint of the whole plan, and P3's named failure mode is *eviction storms
during scene loads becoming disc stalls*. This attacks that failure mode
directly, which licenses a smaller pool. It is the ARAM pager's own design
pattern applied one tier up.

The tier is itself a cache, so it can be trimmed to zero under VRAM pressure and
never needs a budget. Floor it at 0 and size it last.

**This is not MMU paging** — no faults, asset granularity, order-aware,
evictable: the four properties `kb/closed.md` says the pack approach has and
paging lacked.

### R5. AICA spare as a third tier — a repayable bridge, never a line in the inequality.

Same tier interface as R4, backed by G2 DMA into AICA RAM, strictly bulk-copy
and never CPU-addressed — exactly the boundary `kb/closed.md` draws. ~1.9 MB of
the fastest non-main-RAM in the machine is idle while audio is off.

⚠️ **Audio stage B will want this RAM.** Any fit arithmetic that depends on the
AICA tier permanently is self-deception. Label it bridge capacity, repayable on
demand, and never count it.

### R6. Cold `.text` → VRAM. ~~0.5-1.5 MB image-equivalent.~~ The honest replacement for L4.

> 🔴 **[PREMISE VOID 2026-08-06 — R6 SHOULD BE RE-RANKED TO NEARLY LAST.]**
> R6's opening sentence is its whole justification, and it is now false. `.text`
> did not have to move; it had to be compiled. **5,506,964 → 2,753,700 B, a
> −2,753,264 B win, taken by a compiler flag with zero hardware risk** — against
> R6's speculative 0.5-1.5 MB that requires a linker `MEMORY` region at a VRAM
> VMA, a boot-time copy-out ordered before `pvr_init`, a curated cold set, and
> **the one genuinely unverified hardware premise on this page** (SH-4
> instruction fetch from VRAM across Holly, contending with TA traffic).
>
> What replaces the claim: the cheap, safe lever on `.text` is **codegen**, and
> it has been taken. `DC_OPT_PROFILE=size` (flat `-Os`, no `-O3` hot list) is a
> further −72,   ... i.e. `.text` 2,753,700 → 2,680,676 B, and it is a one-word
> build change with a kill switch (`.text` 2,753,700 → 2,680,676 B, at a
> measured cost of 20.6 → 18.5 town FPS) — that is the lever to reach for
> *before* R6.
> R6 survives only as a genuine last resort if the fit fails after every cheaper
> move, and its candidate cold set (T5's 102,648 B floor, `dvderr` ~42 KB, …)
> was itself measured at `-O0` and shrank with the rest of `.text`. Evidence:
> `kb/state-log.md`, 2026-08-06.

~~`-O0` means `.text` (5,804,776 B) can only *move*.~~ VRAM is SH-4 addressable,
cacheable through P1, and the SH-4 can fetch instructions from it. So: a linker
`MEMORY` region at a VRAM VMA, curated cold sections (`-ffunction-sections` is
already on, so this is pure placement) with VMA=VRAM and LMA=image tail, copied
out at boot before `pvr_init` claims the region, and the LMA tail donated to the
heap — T5's donation mechanism generalised.

**Why the MMU-paging closure does not apply:** misclassification costs *latency,
not correctness*. A "cold" function that turns out to be called still runs, just
with I-cache fills from VRAM. There is no loader, no eviction, no fault handler
and no backing-store transfer, so none of the killers in
`kb/research-mmu-paging.md` bite.

Candidate cold set with in-repo evidence: T5's measured 102,648 B floor,
`dvderr`/symbolication ~42 KB, famicom paths, `CARD*`/save paths, error
handlers — plausibly 0.5-1.5 MB curated.

⚠️ **The one genuinely unverified hardware premise on this page:** instruction
fetch from VRAM across Holly, contending with TA and render traffic. Needs a
"execute a loop from VRAM while rendering" probe. Also: `L4` says `.text`
relocation is *not needed* — honour that. This is the contingency if P1/P2
margins erode, not a thing to start while they hold.

### R7. Guardrail: `prbuf` must never come back. −614,368 B of avoided regression.

`prbuf` (614,368 B) is banked *because its writer is a stubbed no-op* — but
`kb/boot-blockers.md` item 8 shows every town menu eventually needs EFB→texture
capture. The naive future fix re-inflates 614 KB of `.bss`.

**The right fix captures the framebuffer VRAM→VRAM** (PVR render-to-texture, or
a straight copy at `PVR_FB_R_SOF1`, which `dc/` already knows how to read), so
the snapshot never transits main RAM. Failure mode: the display is 565 scanout
while the re-bind wants a twiddled texture, and there is no GPU detwiddle — the
copy may need one CPU pass through a small strip buffer.

Banked here as one paragraph so the M2 implementer does not re-inflate it.

### R8. Systematic S7-shape `.data` census. 100-300 KB, speculative.

S7's shape — a `.data` table whose only reader replays it forward once — beat
its estimate (−246,064 B). The shape is mechanically discoverable: for each
large remaining `.data` symbol (post-P8 remainder ~1.3 MB), count readers and
classify access. First-pass candidates: the `cKF_*` keyframe families,
`sys_dynamic`, `*_pal` tables with one `bcopy` consumer. L6's verdict ("120 KB
durable, do after L3 or never") is the cautionary precedent; rank low, but the
census itself is one scripted afternoon.

### R9. Product decisions, stated honestly — the user's call, not engineering's

- **Drop jaudio entirely** (`DC_NO_SOUND` edition): 1,265,101 B `.bss` +
  319,194 B `.text` = **~1.58 MB**, the second-largest single lever in the tree
  after L1. And note what 2026-08-04 measured: sound now works but costs ~45 %
  of the frame rate (`kb/state-log.md`), so this lever now buys RAM *and* FPS.
  Belongs beside L5 in the "documented DC edition" bucket.
  ⚠️ **[STALE 2026-08-06] Both halves of that costing moved and neither has been
  re-taken.** The `319,194 B` of `.text` is an `-O0` figure (whole-image `.text`
  fell 5,506,964 → 2,753,700 B); the `.bss` figure should be unaffected
  (`.bss` moved +128 B in total). The **~45 % of frame rate** was measured with
  jaudio compiled at `-O0`, and jaudio is now `-Os` — see `kb/audio-cpu-cost.md`,
  where that whole cost model is flagged void. **Do not quote this bullet's
  price until an audio-on `perf`-vs-`o0` A/B runs.** It is still the user's
  call, and it is now a call on unknown numbers rather than known ones.
- **Famicom/NES**: already measured at ~39 KB. Not worth the product cost.
  Confirmed, not re-proposed.

---

## Rejected while generating — recorded so nobody re-derives them

- **Link-time `OVERLAY` of scene-exclusive `.bss` destination groups.**
  Mechanically legal, zero codegen — but under P1 the arrays become 8-byte stubs
  and there is nothing left to overlay, and without P1 the mutual-exclusivity
  proof is data-dependent (any of ~230 villagers can appear in any town).
  Strictly dominated by P1. Only exhumable if P1 dies entirely.
- **"Ship the stub build + keep list as S4-lite".** Fails on data-dependent
  working sets: villagers, furniture and patterns are chosen by the save file and
  resolved by index at runtime. No static list exists — the census docs say
  exactly this.
- **The GD-ROM drive buffer as storage.** Not SH-4 addressable in any documented
  way. Dead on arrival.
- **Reclaiming the low 64 KB, or shrinking the 64 KB kernel stack.** The low
  region holds GD-ROM syscall state KOS's cdrom driver uses; the kernel stack is
  a KOS rebuild knob worth ~32 KB at real IRQ-headroom risk. Not worth it.

---

## The single highest-leverage action

## ⭐ 2026-08-04 — bench_mem HAS NOW BEEN BUILT AND RUN, and Flycast cannot answer it

`harness/dc/bench/{Makefile,build.sh}` exist; `bash harness/dc/bench/build.sh`
then `bash harness/dc/smoke.sh ~/.cache/oc-dc-harness/bench/bench_mem.cdi
--timeout 180`. It **passes**: `end_rc=0`, `no_failed_asserts`, every one of the
~56 cases reports `ok` (checksum verified), and no transfer path hangs —
including the `dma_is_running(DMA_CHANNEL_3)` spin and the AICA section running
without `spu_init()`, which were the two pre-run risks.

**But the MB/s column from Flycast is an artefact, and it says so in its own
shape:**

| case | 4 KB | 64 KB | 256 KB |
|---|---|---|---|
| VRAM 32b CPU store32 **W** | 114.3 | 114.3 | 114.3 |
| VRAM 32b CPU load32 **R** | 114.3 | 114.3 | 114.3 |
| VRAM 64b CPU store32 **W** | 114.3 | 114.3 | 114.3 |
| VRAM 64b CPU load32 **R** | 114.3 | 114.3 | 114.3 |

Read equals write, the 32-bit window equals the 64-bit window, and every size
gives the same figure to one decimal. On real hardware the 64-bit area is
roughly twice the 32-bit area and a CPU read from VRAM is far worse than a
write — the whole reason the number was wanted. This is a constant, not a
measurement. The DMA rows are worse: `117,028 MB/s` and `29,257 MB/s` come from
a 2,240 ns or 0 ns sample, i.e. below the timer's resolution.

**So this is now a HARDWARE task, not a Flycast task**, and running it is
de-risked: we know it completes and verifies. A hardware build must first drop
`BENCH_BAUD` from 1,562,500 to 57,600 (`kb/traps.md`: a coder's cable will not
sync at 1.5 Mbps and the console, crash dumps included, is lost).

Until that burn happens, **R1/R3/R4/R5/R6 remain gated** — the same as before,
but for a known reason rather than an unrun benchmark.

<details><summary>the original item, and the rest of the flycast numbers</summary>

Main-RAM `memcpy` reads 130-134 MB/s at all three sizes, and the AICA CPU paths
order themselves plausibly (G2 PIO 38-47, store queues 395-398), so the
instrument is not returning garbage everywhere — it is specifically the VRAM
windows that Flycast flattens.

**Build and run `harness/dc/bench/bench_mem.c`.** It exists and has never been
run. It gates R1, R3, R4, R5 and R6 simultaneously and settles the project's one
missing number — SH-4-from-VRAM read bandwidth, in both directions, plus G2 — in
half a day.

</details>
