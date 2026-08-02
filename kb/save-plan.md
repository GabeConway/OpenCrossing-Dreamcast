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
