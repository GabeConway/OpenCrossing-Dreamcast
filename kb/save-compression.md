# Save compression measurements and the VMU fit verdict

§2 and §3 of `kb/save-budget.md`, moved verbatim: codec comparison, where the
compressed bytes go, the fit table against 200 VMU blocks, and the measured
PLAN §6 options (2 VMUs / trim / delta). Read when choosing a codec or arguing
about whether a save fits.
**All compression numbers here are SYNTHETIC** — no real `.gci` exists on this
machine; see the warning in `kb/save-budget.md`.

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
