# Save — the recommended VMU plan, write cost, and open items

§4, §5, §6 and the sources of `kb/save-budget.md`, moved verbatim: the shipping
recommendation (locker trim + chunked deflate-6 + budget/eviction + spill),
the compression and VMU-flash cost bounds, and what still has to be measured.
Read before implementing the VMU backend.
**Both halves of the write-cost section are estimates, and the compression
figures they rest on are SYNTHETIC** (`kb/save-budget.md`).

---

## 4. Recommendation

**Ship: one dedicated VMU, DC-edition locker trim, chunked deflate-6, hard
budget with priority eviction, automatic spill to a second VMU when present.**

`savebench.py --recommend`, chunk size 16 KB:

| profile | untrimmed blocks | **DC-edition trim** | % of one VMU |
|---|---:|---:|---:|
| fresh | 30 | **22** | 11% |
| typical | 111 | **84** | 42% |
| full | 203 (over) | **150** | 75% |
| adversarial | 410 (over) | 259 (over) | 130% |

Concretely:

1. **Payload = `Save_t` + the three keep-blocks. Drop the GC banner/icon, the
   sector padding, and the backup copy.** 456 KB -> 289 KB for free. Integrity
   comes from a CRC over the payload plus the VMS header's own CRC field, not
   from a second copy we cannot afford.

2. **DC-edition locker trim: `keep_original` 96 -> 24 designs (2 pages),
   `keep_mail` 160 -> 40 letters (2 pages), `keep_diary` 48 -> 12 entries
   (1 player-year).** 295,910 -> 185,238 raw. Document it loudly as a DC-edition
   cut. It is a serialisation-capacity change only — the in-RAM structs stay
   the size the game code expects, so no decomp surgery.

3. **zlib deflate level 6, in independent 16 KB chunks** with a small chunk
   index. Chunking costs +3.8% at the `full` point (76,067 vs 73,642 B) and buys
   byte-stable output for unchanged chunks, which is the only way to make
   incremental VMU writes possible — a whole-stream deflate shifts every byte
   after an edit. zlib is already on the kos-ports list (PLAN.md §8).

4. **Enforce a hard block budget (suggest 180 of 200) at save time.** Compress;
   if over budget, evict in a fixed priority order and recompress:
   `keep_diary` -> `keep_mail` pages -> `keep_original` pages -> oldest
   villager memories. Warn the player once. **A save must never fail** — the
   base port's rule that the only silent path through the writer is
   `!pc_save_ready` (kb/game.md) applies here too, and the DC has a much
   smaller error budget.

5. **Automatic spill to VMU slot 2** when the compressed payload exceeds the
   budget and a second VMU is present: part 1 to A1, part 2 to A2, cross-linked
   by a token in both headers so a half-pair is detected on load. This turns the
   adversarial case (259 blocks) into a fit without demanding two VMUs from
   anyone who does not need them.

6. **Keep GCI import/export lossless via a host tool** (PLAN.md §6). The on-VMU
   format is a storage encoding; the interchange format stays the 467,008-byte
   `.gci` so towns move DC <-> Dolphin <-> the Anbernic port. The host tool is
   also where a trimmed DC save gets re-expanded to full locker capacity.

Explicitly rejected: LZMA/bzip2 (14% better at best, far more SH-4 time and
RAM), zstd (needs ~1 MB of compression context on a 16 MB machine for a
marginal win), preset dictionaries (measured at -0.7%), delta/journal as a
footprint strategy (does not reduce footprint), and requiring 2 VMUs by default.

---

## 5. Write cost

`savebench.py --cost`. **Both halves of this section are estimates and must be
re-measured** — the compression side on Flycast/hardware, the VMU side on real
hardware with a real VMU.

**Compression.** Host measurements on the 289 KB payload, Apple M4:

| level | host time | throughput | SH-4 extrapolation (40-100x) |
|---|---:|---:|---|
| deflate-1 | 3.5 ms | 83 MB/s | 0.14 - 0.35 s |
| **deflate-6** | 13.8 ms | 21 MB/s | **0.55 - 1.38 s** |
| deflate-9 | 53.8 ms | 5.5 MB/s | 2.15 - 5.38 s |

The 40-100x scale factor is a guess, not a measurement. Even at the pessimistic
end deflate-6 is acceptable: the save flow already parks the player in a
dedicated scene (`SCENE_PLAYERSELECT_SAVE`, `m_scene.c:354` -> restart-NPC
dialogue -> `mCD_SaveHome_bg`, `pc/src/pc_m_card.c:989` — see kb/game.md) with
several seconds of animation to hide behind.

**VMU flash.** Two hard bounds, and they are far apart:

- *Floor:* a VMU cannot accept successive block-write packets faster than
  ~10 ms ([dreamcast.wiki](https://dreamcast.wiki/VMU_peripheral)) -> 100 blk/s.
- *Ceiling:* KOS writes a 512 B block as **5 dependent maple transactions** —
  four 128 B phases plus a `MAPLE_COMMAND_BSYNC` completion
  ([KallistiOS `hardware/maple/vmu.c`](https://github.com/KallistiOS/KallistiOS/blob/master/kernel/arch/dreamcast/hardware/maple/vmu.c)).
  If each costs one maple DMA cycle at the ~16 ms controller-poll cadence, that
  is ~83 ms/block -> ~12 blk/s.

| write | optimistic (10 ms/blk) | pessimistic (5 x 16.7 ms/blk) |
|---|---:|---:|
| full save, 150 blocks | 1.5 s | 12.5 s |
| typical save, 84 blocks | 0.8 s | 7.0 s |
| incremental, ~20 blocks | 0.2 s | 1.7 s |

**The pessimistic bound is the reason for chunking.** A 12.5 s "please do not
turn off" is unacceptable for a game the player saves every session. With
16 KB chunks, a day of play dirties a handful of them (§3.1 measured 4 of 577
512 B pages at 0.5% churn), so a normal save rewrites ~20 blocks, not 150.
Full rewrites happen on first save and after a big session.

**Action:** measure the real per-block cost early — it is a ~30-line KOS test
and it decides whether incremental writes are a nice-to-have or mandatory. Put
it in `harness/dc/` at M0/M1.

---

## 6. Open items

1. **Get a real late-game `.gci` and re-run `--gci`.** Everything in §2-§4 is
   synthetic. The two parameters the answer is most sensitive to are design-art
   entropy and letter-text entropy; both are guessed here.
2. **Measure VMU block-write time on hardware** (§5). Decides chunk size and
   whether incremental writes are mandatory.
3. **Measure deflate-6 throughput on SH-4** in Flycast at M3. Replaces the
   40-100x hand-wave.
4. **Design dedup.** Players copy the same pattern into several slots, and
   Able's shop display often mirrors a player slot. A content-hash dedup pass
   before compression is free and could matter a lot in exactly the maxed case
   that is tight. Untested — no corpus.
5. **Confirm the KOS maple DMA cadence.** The pessimistic bound assumes one
   dependent transaction per ~16 ms cycle; if KOS can issue several per cycle
   to the same device, the write cost drops by up to 5x and chunking becomes
   optional.
6. **Decide the eviction priority order** with the game designer's hat on
   (§4.4) and make sure the eviction is visible to the player, not silent.

## Sources

- VMS/VMU file header layout, eyecatch sizes: [Dreamcast Programming — VMS File Header](https://mc.pp.se/dc/vms/fileheader.html)
- VMU capacity, ~10 ms/block write floor, ~16 ms poll cadence: [dreamcast.wiki — VMU peripheral](https://dreamcast.wiki/VMU_peripheral)
- 4 x 128 B phases + BSYNC per block write: [KallistiOS `kernel/arch/dreamcast/hardware/maple/vmu.c`](https://github.com/KallistiOS/KallistiOS/blob/master/kernel/arch/dreamcast/hardware/maple/vmu.c)
- Save-flow call chain: `kb/game.md`, `pc/src/pc_m_card.c:989`, `include/m_scene.h`
- All struct sizes/offsets: `tools/savebench/save_layout_probe.c` output, cross-checked against the offset comments in `include/m_common_data.h`, `include/m_card.h`, `include/m_private.h`, `include/m_home_h.h`, `include/m_npc.h`, `include/m_mail.h`, `include/m_needlework.h`, `include/m_diary.h`, `include/m_island.h`


## 7. Implementation status — MEASURED 2026-08-02

Everything above §7 is host-side modelling. This section is what the Dreamcast
actually did. Written after implementing the VMU backend in `dc/src/dc_card.c`.

> **Where this section belongs.** In the split layout (`save-layout.md` /
> `save-compression.md` / `save-plan.md`) this is `save-plan.md` §7 and it
> supersedes that file's §5 write-cost estimates and §6 open items 2, 3 and 5.
> It is appended here because the worktree it was written in predates the
> split.

### 7.1 What is implemented

`dc/src/dc_card.c` + `dc/include/dc_card.h`. A real KOS `vmufs`/`vmu_pkg`
backend under the 29 `CARD*` entry points, plus a storage API
(`dc_save_store` / `dc_save_load` / `dc_save_erase` / `dc_save_free_blocks` /
`dc_save_present`) that does not require the 20-byte `CARDFileInfo` dance.

On-VMU format, the **OCS1 container** (byte table in `dc/include/dc_card.h`):
a 640 B VMS header (1 icon, **no eyecatch**) from `vmu_pkg_build`, then a 32 B
little-endian header, a `u32`-per-chunk length table, and independent 16 KB
zlib deflate-6 streams. `crc32` over the raw payload *and* over the stored
bytes; the VMS header carries its own CRC16. Two integrity gates, because §4.1
spends the GameCube's backup copy and detection has to replace redundancy.
On-card filename `OPENXING.SAV`, app id `OPENCROSSING` — VMU dirents are 12
chars, so `DobutsunomoriP_MURA` cannot be stored and GCI naming survives only
in the host interchange tool.

Kill switches, all compile-time: `DC_CARD_DISABLE`, `DC_CARD_READONLY`,
`DC_CARD_NO_COMPRESS`, `DC_CARD_NO_CHUNK`, `DC_CARD_SELFTEST=0`,
`DC_CARD_BENCH`. The reader always accepts both stored and deflated chunks, so
a save written with compression off still loads with it on.

### 7.2 Proof

Harness: `tools/savebench/dcvmu/` — the real `dc_card.c` compiled with
`-DDC_CARD_STANDALONE` into a 2.3 MB KOS program that boots in Flycast. It
exists because the game image is 21.4 MB against a 16 MB machine and prints
zero bytes (`kb/STATE.md`), so the save path cannot be observed from a game
build at all. Same file, same code path; when the image fits, the identical
`dc_card_selftest()` runs from `CARDInit()` with no change.

Console, Flycast v2.6 / KOS 2.3.0 `1c6398f`, two VMUs (`A1`, `A2`):

```
[DC/CARD] VMU 0 at A1 'Visual Memory   '  200 free blocks
[DC/CARD] SELFTEST PASS: 8192 B round-tripped byte-exact (packed 3528 B = 7 blocks, 3% of a VMU)
[DC/CARD] SELFTEST foreign-file: rc=-6 (file is not an OpenCrossing save) — PASS
[DC/CARD] SELFTEST corrupt-file: rc=-5 (corrupt (CRC mismatch)) — PASS
[DC/CARD] SELFTEST missing-file: rc=-2 (no save file on that VMU) — PASS
[DC/CARD] SELFTEST vmu-full: rc=-3 (VMU full), free 198 -> 198 — PASS (nothing written)
ASSERT ok sector8k
ASSERT ok keepmail
ASSERT ok vmufull100k
OC-DC-HARNESS-END rc=0
```

**Independently re-verified on the host.** `tools/savebench/dcvmu/vmu_extract.py`
walks the 128 KB flash image Flycast wrote, follows the FAT chain, recomputes
the VMS CRC16 and both container crc32s:

```
VMU vmu_save_A1.bin: 200 user blocks, 1 entries, 69 blocks used, 131 free
  OXRT.TMP      data  69 blk ( 35328 B)  app_id=OPENCROSSING  payload= 34564 B  VMS crc OK
      OCS1: 102400 B raw -> 34504 B stored in 7 x 16384 B chunks (2.97:1, deflate), both crc32 OK
```

A guest that lies and a host parser that lies the same way is not a plausible
pair of bugs, so the round trip is real.

### 7.3 The four failure modes, all exercised

| mode | behaviour | proven by |
|---|---|---|
| **no VMU** | `DC_SAVE_ENODEV` → `CARD_RESULT_NOCARD`. No retry, no crash. A Dreamcast with no memory card is a Dreamcast, not an error. | the `no-vmu(chan1)` case when only one card is attached; `CARDMount`/`CARDProbe`/`CARDCheck` all report NOCARD |
| **VMU full** | pack in RAM, count blocks — crediting back our own existing file, which `VMUFS_OVERWRITE` frees — and **refuse before writing**. The old save is never deleted for a new one that will not fit. | `vmu-full`: 110 KB of PRNG → "needs 222 blocks, 200 available", free count unchanged 198 → 198 |
| **foreign file** | valid VMS header and CRC, someone else's `app_id` → `DC_SAVE_EFOREIGN`, left untouched | `foreign-file`, written with `app_id="SOMEOTHERGAME"` |
| **corrupt file** | our `app_id`, VMS CRC recomputed over the damaged bytes so it *passes*, one payload byte flipped → caught by the OCS1 `crc32` | `corrupt-file` |

`CARDFormat` is **refused**, not implemented: on a GameCube it formats one
memory card; on a Dreamcast it would erase every other game's save on the VMU.

### 7.4 Measured write and read cost — §5's pessimistic bound is the right one

Three round-trips, all byte-exact, on Flycast:

| payload | on card | blocks | write | read |
|---:|---:|---:|---:|---:|
| 8,192 B (one GC sector) | 3,525 B | 7 | 1.270 s | 0.393 s |
| 47,780 B (`mCD_keep_mail_c`) | 16,853 B | 33 | 3.458 s | 0.837 s |
| 102,400 B (a whole VMU) | 35,204 B | 69 | 6.516 s | 1.457 s |

Both are cleanly linear in blocks (three-point fit, max residual 0.3%):

```
write ≈ 0.678 s + 84.6 ms/block
read  ≈ 0.273 s + 17.2 ms/block
```

**84.6 ms/block lands within 1.3% of §5's pessimistic ceiling** — 5 dependent
maple transactions × the ~16.7 ms poll cadence = 83.5 ms/block, derived from
KOS's `vmufs.c`. The optimistic 10 ms/block floor from dreamcast.wiki does
**not** describe KOS's path. Flycast evidently models the maple poll cadence;
that is not the same as modelling flash-write latency, so a real VMU can only
be slower than this, never faster.

Extrapolated:

| save | blocks | write | read |
|---|---:|---:|---:|
| §4 "typical", DC-edition trim | 84 | **7.8 s** | 1.7 s |
| §4 "full", DC-edition trim | 150 | **13.4 s** | 2.9 s |
| §3 "full", untrimmed | 195 | **17.2 s** | 3.6 s |

**This makes incremental writes mandatory, and chunking as shipped does not
deliver them.** The 16 KB chunk layout gives byte-stable output for unchanged
chunks, which is the precondition §4.3 wanted — but `vmufs_write()` rewrites
the whole file regardless, so today a one-item save still costs the full 13 s.
Turning the format's property into a saving needs a writer that diffs the
packed blob against what is on the card and rewrites only the dirty 512 B
blocks in place, through the low-level `vmufs_file_*`/FAT calls. That is the
next piece of work on this path and it is worth ~10 s per save.

### 7.5 Measured deflate-6 throughput — the 40-100× hand-wave was 4-11× too pessimistic

```
[DC/CARD] BENCH deflate-6 295910 B -> 99657 B in 128571 us (2301 KB/s) => 196 VMU blocks
```

§5 extrapolated 0.55-1.38 s for the 289 KB payload from a 21 MB/s host
measurement and a guessed 40-100× slowdown. The measurement is **0.129 s** — a
~9× slowdown, not 40-100×. **Compression is free**: 0.13 s against 13.4 s of
flash writing. It is not worth optimising and it is not worth trading ratio for
speed. deflate-9 is still the wrong pick, but for §2's reason (0.6% better
output), not for CPU.

⚠ Flycast's SH-4 timing is approximate. Take 2.3 MB/s as an order of magnitude,
not a spec.

### 7.6 What the compression numbers here do and do not mean

The ratios above — 2.32:1 at 8 KB, 2.83:1 at 47 KB, 2.97:1 at 100 KB and at
295 KB — are of a **synthetic three-regime test pattern**: a third zeros, a
third cyclic ASCII, a third PRNG, chosen so a save's three content kinds are
all represented and so the incompressible third cannot be faked away. They
prove the codec path works end to end. **They say nothing about how a real town
compresses.** §2's warning stands in full, and §6.1 (get a real late-game
`.gci`) is still the single highest-value open item on the save path.

That the bench's 196 blocks sits next to §3's synthetic "full" 195 blocks is a
coincidence of two unrelated synthetic constructions, not corroboration.

### 7.7 What it costs the image

Measured against the same build with the previous stub `dc_card.c`
(`DC_ASSET_STUB=1`, `DC_ARAM_WINDOW=851968`, `DC_ARENA_BYTES=1900000`):

| | before | after | delta |
|---|---:|---:|---:|
| `.text` | 6,318,568 | 6,360,284 | **+41,716** |
| `.data` | 2,638,852 | 2,638,852 | 0 |
| `.bss` | 12,415,508 | 12,415,316 | **−192** |
| image span | 21,372,928 | 21,414,452 | **+41,524** |

The `.text` is almost entirely zlib being pulled in for the first time —
`inflate.o` 5,332 B, `deflate.o` 3,232 B, `crc32.o` 744 B and the trees/inffast
tables behind them. `-lz` was always on the link line but `--gc-sections` had
been dropping all of it. 41.5 KB is 0.44% of the ~9.5 MB the image is over
(`kb/STATE.md`); `DC_CARD_NO_COMPRESS=1` recovers most of it and is the switch
to reach for if the last megabyte ever comes down to this.

Transient RAM during a save: the packed buffer (`compressBound` of the payload)
+ the `vmu_pkg_build` output + zlib's ~256 KB deflate state, on top of whatever
the caller already holds. For the 289 KB payload that is roughly 0.9 MB of peak
transient. Not resident, but not nothing on a 16 MB machine — and it is
*additional* to the 466,944 B staging buffer `CARDOpen` allocates.

### 7.8 The gap: the game does not route its save through any of this

**`pc/src/pc_m_card.c` does not use the `CARD*` API for the town.** It does its
save I/O with `<stdio.h>` against the relative path
`save/card_a/DobutsunomoriP_MURA.gci` (`pc_save_write_gci_ex`,
`pc_save_read_gci`, `pc_save_check_and_load`). Of the 29 `CARD*` entry points
only `CARDInit()` is on its path, plus `pc_card_scan_for_gci()`. So the console
line the port reaches today — `[PC] No save file found` — comes from a failed
`stat()`, and making `dc_card.c` real does not by itself change it.

`pc_card_scan_for_gci()` therefore still returns 0 even when a save IS on the
VMU, and logs that fact rather than hiding it. Returning a path would make
`pc_m_card.c` print "found" and then fail to `fopen` it, which is strictly
worse than "no save".

**The missing piece, and it is a bounded one:** a KOS `vfs_handler_t`
registered at, say, `/dcsave`, presenting the fixed namespace
`save/card_a/DobutsunomoriP_MURA.gci` (+ `.tmp`, `.bak1..3`) and `save/card_b/`,
with `fs_chdir("/dcsave")` at boot so the relative paths resolve. Reads
decompress from the VMU into a RAM image on first open; the writer buffers in
RAM and **commits on `rename()`** — which is exactly what
`pc_save_write_gci_ex` does as its last step (`rename(tmp_path, gci_path)`) and
which a VFS handler receives as its `->rename` callback. That is an atomic
commit point for free, through the documented KOS API, with no symbol
interposition and no edit to `pc/`.

Two reasons it is not in this change: it needs an `fs_chdir()` from
`dc/src/dc_main.c`, owned by another work stream at the time, and it wants to
be verified against a booting image rather than a standalone harness.

### 7.9 Open items, updated

| § | item | status |
|---|---|---|
| 6.1 | real late-game `.gci` | **still open, still the top item.** Every ratio in this document is synthetic |
| 6.2 | VMU block-write time | **answered under emulation** (§7.4): 84.6 ms/block, matching the pessimistic model. Still wants a real VMU |
| 6.3 | deflate-6 throughput on SH-4 | **answered** (§7.5): 2.3 MB/s, ~9× the host, i.e. free |
| 6.4 | design dedup | untouched |
| 6.5 | maple DMA cadence | **answered by implication** (§7.4): the 5-transactions-per-block model is right; KOS does not batch |
| 6.6 | eviction priority order | **not implemented.** `dc_save_store` refuses when full; it does not evict. §4.4's priority ladder and the player-visible warning are still to build |
| new | incremental block-level writes | the ~10 s/save win in §7.4. Needs a diffing writer over `vmufs_file_*` |
| new | the stdio→VMU VFS bridge | §7.8. Without it nothing above reaches the game |
| new | spill to a second VMU (§4.5) | not implemented. Channel 1 maps to the second VMU and reads/writes fine; the split-across-two-cards logic does not exist |

## Sources

- VMS/VMU file header layout, eyecatch sizes: [Dreamcast Programming — VMS File Header](https://mc.pp.se/dc/vms/fileheader.html)
- VMU capacity, ~10 ms/block write floor, ~16 ms poll cadence: [dreamcast.wiki — VMU peripheral](https://dreamcast.wiki/VMU_peripheral)
- 4 x 128 B phases + BSYNC per block write: [KallistiOS `kernel/arch/dreamcast/hardware/maple/vmu.c`](https://github.com/KallistiOS/KallistiOS/blob/master/kernel/arch/dreamcast/hardware/maple/vmu.c)
- Save-flow call chain: `kb/game.md`, `pc/src/pc_m_card.c:989`, `include/m_scene.h`
- All struct sizes/offsets: `tools/savebench/save_layout_probe.c` output, cross-checked against the offset comments in `include/m_common_data.h`, `include/m_card.h`, `include/m_private.h`, `include/m_home_h.h`, `include/m_npc.h`, `include/m_mail.h`, `include/m_needlework.h`, `include/m_diary.h`, `include/m_island.h`
