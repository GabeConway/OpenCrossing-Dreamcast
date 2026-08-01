# Save budget: 289 KB of Animal Crossing into a 100 KB VMU

Measured 2026-08-01 (M1, PLAN.md §6 / risk "Save won't fit VMU").
Harness: `tools/savebench/` — re-run it whenever the save layout changes.

**Verdict up front:** the uncompressed unique payload is **295,910 bytes**
(289 KiB) — not the 456 KB the GC card file suggests, because a third of that
file is a backup copy and another chunk is GC banner/icon/padding. With the
proposed shipping encoder (chunked deflate-6) a *typical* save fits one VMU
comfortably (**111 of 200 blocks**); a *maxed* save does not fit with any
safety margin (**203 blocks, 101%**). The fix is not a better compressor — it
is trimming the three storage-locker blocks, which drops the maxed case to
**150 blocks (75%)**. The adversarial ceiling still overflows one VMU, so the
save path must also degrade gracefully rather than fail.

> **All compression numbers on this page are SYNTHETIC.** No real `.gci` exists
> on this machine (searched `OpenCrossing-Anbernic/**` including
> `pc/build-*/bin/save/card_a|card_b`, `harness/out/`, and git history — the
> card dirs exist but are empty; the only artefact in history is
> `harness/inspect-gci.py`). The *layout* numbers are exact and verified. The
> *content* model is an educated construction. Re-run with `--gci` against a
> real late-game save before treating any ratio as settled.

---

## 1. What the save actually is

Verified by compiling `tools/savebench/save_layout_probe.c` against the
vendored decomp headers for a 32-bit ARM EABI target; the resulting offsets
reproduce the `/* 0x...... */` comments in `include/m_common_data.h` byte for
byte. (A native 64-bit host build reports `sizeof(Save_t) == 153216` — wrong,
inflated by LP64 alignment. Always use the armhf container; see the harness
README.)

| symbol | bytes | hex | source |
|---|---:|---|---|
| `sizeof(Save_t)` | 148,128 | 0x242A0 | `include/m_common_data.h:81-174` |
| `sizeof(Save)` (sector-aligned) | 155,648 | 0x26000 | `include/m_common_data.h:176-179` |
| `sizeof(mCD_others_c)` | 153,056 | 0x255E0 | `include/m_card.h:179-185` |
| `OTHERS_SIZE` (sector-aligned) | 155,648 | 0x26000 | `include/m_card.h:287` |
| `mCD_LAND_SAVE_SIZE` | 466,944 | 0x72000 | `include/m_card.h:17` |
| `mCD_MEMCARD_SECTORSIZE` | 8,192 | 0x2000 | `include/m_card.h:128` |

The `.gci` the port writes (`pc/src/pc_m_card.c:56-61`, `:322-335`):

```
0x000000     64  CARDDir header
0x000040 0x26000 mCD_others_c   (GC banner/icon + the three "keep" lockers)
0x026040 0x26000 Save  main
0x04C040 0x26000 Save  backup   <- byte-identical duplicate of the above
        ---------
        467,008 B on disk / 466,944 B on card = 57 x 8 KB blocks
```

Of those 456 KB, only **295,910 bytes** are unique data the Dreamcast has to
keep:

| block | bytes | note |
|---|---:|---|
| `Save_t` | 148,128 | the town |
| `mCD_keep_original_c` | 52,384 | 96 stored design patterns (8 pages x 12) |
| `mCD_keep_mail_c` | 47,780 | 160 stored letters (8 pages x 20) |
| `mCD_keep_diary_c` | 47,618 | 48 diary entries (4 players x 12 months) |
| **unique payload** | **295,910** | **289.0 KiB** |

Everything else in the 456 KB is dead weight on DC: the 5,184 B GC
comment/banner/icon (`MemcardHeader_c`, `include/m_card.h:131-135` — replaced
by a 640 B VMS header), ~15 KB of sector-alignment padding, and the 155,648 B
backup copy. **Dropping those three costs nothing and is the first 35% saved.**

### 1.1 Byte breakdown by field group

From `savebench.py --groups`. Group membership is defined in
`tools/savebench/save_layout.py`, which tiles all four blocks exactly (no gaps,
no overlap — asserted).

| group | raw bytes | % of payload | what it is |
|---|---:|---:|---|
| designs | 74,528 | 25.2% | **137 design slots x 544 B.** 96 in the locker + 8 per player x 4 + 8 Able Sisters + 1 island flag. Each = 16 B name + palette + 512 B of 32x32 4bpp art (`include/m_needlework.h:56-61`) |
| letters | 71,752 | 24.2% | **240 `Mail_c` x 298 B.** 160 locker + 10 inventory x 4 players + 10 mailbox x 4 houses. 248 B of text each (`include/m_mail.h:99-104`) |
| keep_diary | 47,618 | 16.1% | 48 x 992 B of generated prose (`include/m_diary.h:12-16`) |
| villagers | 39,040 | 13.2% | 15 x `Animal_c` (2,440 B). 2,184 B of that is `memories[7]`, each carrying a 262 B saved letter (`include/m_npc.h:177-186, 200-230`) |
| house_layout | 28,840 | 9.7% | 4 houses x 3 rooms x 4 furniture layers, each a 16x16 u16 item grid, + the island cottage (`include/m_home_h.h:132-151`) |
| town_items | 16,320 | 5.5% | `fg[6][5]` = 30 acres x 16x16 u16 item ids (15,360) + `deposit` buried-item bitfield (960) |
| player_misc | 6,496 | 2.2% | ids, quests, fortune, calendar, e-Card bits |
| town_misc | 4,625 | 1.6% | shop, stalk market, events, post office, police box, snowmen, fish records, mask cat |
| noticeboard | 3,000 | 1.0% | 15 x 200 B bulletin-board posts |
| island | 1,168 | 0.4% | island name/acres/flags (its cottage, islander and flag design are counted in their own groups) |
| house_misc | 1,104 | 0.4% | ownership, HRA marks, gyroid messages, music box |
| catalog | 880 | 0.3% | furniture/wall/carpet/paper/music collection bitfields, 4 players |
| player_items | 184 | 0.1% | 15 pocket slots + equipment, 4 players |
| museum | 63 | 0.0% | `mMmd_info_c` display bits |
| keep_* headers | 260 | 0.1% | folder names, checksums |
| header | 32 | 0.0% | `mFRm_chk_t` + scene/counters |

Two groups — **designs and letters — are half the save**, and they are the only
two the player authors freely. Everything else is bounded, structured, and
compresses well.

---

## 2. Compression measurements (synthetic)

`savebench.py`, whole payload as one stream, four fill profiles. `blocks` =
`ceil((compressed + 640 B VMS header) / 512)`; the VMU user area is
**200 blocks / 102,400 bytes**.

Required ratio for one VMU: **2.91:1**.

| codec | fresh | typical | full | adversarial |
|---|---:|---:|---:|---:|
| store | 580 blk | 580 blk | 580 blk | 580 blk |
| deflate-1 | 32 | 117 | 216 | 415 |
| **deflate-6** | **27** | **104** | **195** | **410** |
| deflate-9 | 25 | 102 | 194 | 410 |
| bzip2-9 | 19 | 87 | 173 | 409 |
| lzma-9e | 21 | 87 | 167 | 392 |
| ratio (deflate-6) | 23.3x | 5.63x | 2.98x | 1.41x |

Profile meanings: `fresh` = post-tutorial town; `typical` = one well-played
player a season in; `full` = 4 players with every slot used and plausible
hand-drawn art; `adversarial` = same occupancy but every user-authored byte
uniformly random (the incompressible ceiling, not a realistic save).

**Findings:**

- **deflate-9 buys 0.6% over deflate-6 for ~4x the CPU.** Use deflate-6.
- **LZMA buys 14% over deflate at the `full` point** (167 vs 195 blocks). Not
  enough to change any verdict, and it costs far more SH-4 time and RAM.
  bzip2 is worse than LZMA and far slower. **zlib (already a kos-port) wins.**
- **A CD-resident preset dictionary is worthless** (`--dict`): -0.7% at best,
  sometimes negative. The save has almost no cross-save constant structure that
  a 32 KB dictionary catches which the 32 KB deflate window does not.
- **The whole spread between deflate-1 and lzma-9e is smaller than the spread
  between profiles.** Content, not codec, decides this.

### 2.1 Where the compressed bytes go

`savebench.py --groups full` (groups compressed separately, so these
over-count relative to the single-stream figure; the *shares* are the point):

| group | raw | deflate-9 | ratio | share of compressed |
|---|---:|---:|---:|---:|
| designs | 74,528 | 21,599 | 3.45x | 22.7% |
| house_layout | 28,840 | 17,667 | 1.63x | 18.5% |
| letters | 71,752 | 15,819 | 4.54x | 16.6% |
| villagers | 39,040 | 14,453 | 2.70x | 15.2% |
| town_items | 16,320 | 9,436 | 1.73x | 9.9% |
| keep_diary | 47,618 | 7,757 | 6.14x | 8.1% |
| everything else | 17,812 | 8,553 | — | 9.0% |

And in the adversarial case:

| group | raw | deflate-9 | ratio |
|---|---:|---:|---:|
| designs | 74,528 | 74,553 | **1.00x** (expands) |
| letters | 71,752 | 42,529 | 1.69x |
| villagers | 39,040 | 25,903 | 1.51x |
| keep_diary | 47,618 | 21,772 | 2.19x |

**The hard floor is the design art.** 137 slots x 512 B = **70,144 bytes of
4bpp pixel data that cannot be compressed below itself** if the player draws
high-entropy patterns. That alone is 137 blocks — 68.5% of a VMU — before any
other byte of the save. This is the single number that decides the design.

Item grids (`house_layout`, `town_items`) compress poorly (1.6-1.7x) even in
the realistic case, because a u16 item id per cell in a filled house is
genuinely near-random. Text (letters, diary, noticeboard) compresses well
(3.9-6.1x) because AC letters share a vocabulary and are zero-padded.

---

## 3. Fit verdict against the VMU

VMU: 256 physical blocks x 512 B; **200 usable for user data = 102,400 B**
(the rest is root/FAT/directory). VMS header with 1 icon and no eyecatch =
128 B header (incl. the 32 B icon palette) + 512 B icon frame = **640 B**.
Eyecatch would cost another 2,048-8,064 B — **do not ship one.**

| case | deflate-6 | blocks | one VMU? |
|---|---:|---:|---|
| fresh | 12,717 | 27 | yes, 13% |
| typical | 52,574 | 104 | yes, 52% |
| **full (plausible max)** | **99,161** | **195** | **97.5% — no usable margin** |
| adversarial | 209,217 | 410 | no; needs > 2 VMUs |

So option (a) from PLAN.md §6 — *single VMU, compressed, unchanged content* —
**technically passes for most saves and fails exactly where it matters**: the
long-running town that the player cares most about. 195/200 blocks is not a
shipping margin; it is a bug report waiting for the player who fills their last
pattern slot.

### 3.1 The other PLAN.md §6 options, measured

**(b) Span 2 VMUs.** 400 blocks. Covers `full` easily and *almost* covers
`adversarial` (410). Works, but it demands the player dedicate both controller
slots — no rumble pack, no second VMU for anything else — and it doubles the
number of ways a save can half-succeed. **Keep as an automatic bonus, not as
the primary plan.**

**(c) Trim content** (`savebench.py --trim`). Measured, deflate-9:

| variant | raw | full | adversarial |
|---|---:|---:|---:|
| nothing trimmed | 295,910 | 194 blk | 408 blk |
| drop `keep_diary` | 248,292 | 180 | 366 |
| drop `keep_diary` + `keep_mail` | 200,512 | 158 | 310 |
| **lockers 8 pages -> 2, diary 1 yr** | **185,238** | **145** | **259** |
| ... and no diary at all | 173,332 | 141 | 248 |
| drop all three keep-blocks | 148,128 | 127 | 207 |

This is by far the strongest lever, and it is cheap: the three keep-blocks are
**storage lockers**, not live game state. `mCD_keep_original_c` is the pattern
storage at the post office, `mCD_keep_mail_c` the letter storage, and
`mCD_keep_diary_c` the diary archive (`include/m_card.h:142-177`). Cutting them
from 8 pages to 2 reduces capacity, not capability: the player's 8 live pattern
slots, their 10-letter inventory, every mailbox, every villager and every
furniture layer are untouched.

Note the floor: even `Save_t` completely alone is **207 adversarial blocks**.
No amount of locker trimming makes one VMU provable in the adversarial case,
because the 41 non-locker design slots (32 player + 8 Able + 1 flag =
22,304 B) plus 80 live letters plus 105 villager-memory letters plus the
furniture grids are already past 100 KB when incompressible.

**(d) Base + delta/journal** (`savebench.py --delta`). Measured on `full`:

| churn | dirty 512 B pages | full re-deflate | dirty pages deflated | XOR-delta deflated |
|---|---:|---:|---:|---:|
| 0.5% | 4 / 577 | 99,400 | 1,496 | 1,415 |
| 2% | 8 / 577 | 100,305 | 2,866 | 2,509 |
| 10% | 58 / 577 | 108,817 | 21,338 | 15,106 |

**A journal solves the wrong problem.** The base image still has to live on the
VMU in full, so the footprint is unchanged; a growing journal makes it *worse*
until compaction. What the numbers do show is that a day of play touches very
little of the save — which is the argument for making *writes* incremental
(§5), not for a delta encoding of the payload.

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
