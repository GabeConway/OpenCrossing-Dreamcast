# dcasset — GameCube ISO → Dreamcast disc layout

Host-side asset extractor. The Anbernic/PC port parses the GameCube ISO **at
runtime** (`pc/src/pc_disc.c` + `pc/src/pc_dvd.c`). The Dreamcast cannot: a
stock console has 16 MB of RAM and reads its own ISO9660 filesystem off CD-R
at ~500 KB/s. So all disc parsing moves offline into this tool, which emits a
plain directory tree that `mkdcdisc` turns into a bootable CDI.

**The user's ISO is read only.** Nothing here writes to it, nothing copies it
into the repo, and `extract` **refuses** an output path inside the repository.
Default output is `/tmp/opencrossing-dc/discroot` (override with `--out` or
`$DCASSET_OUT`).

---

## Headline measurement — the 1.46 GB image is 98.11% zero padding

Measured 2026-08-01 against the user's image
`GAFE01`, disc 0, rev 0, `AnimalCrossing`, 1,459,978,240 bytes, plain ISO/GCM.

| | bytes | | share of image |
|---|---:|---:|---:|
| total image | 1,459,978,240 | 1392.34 MB | 100% |
| GC system area (boot+bi2+apploader+**main.dol**+FST) | 1,041,766 | 0.99 MB | 0.07% |
| FST file content (10 files) | 26,531,779 | 25.30 MB | 1.82% |
| region overlap (counted once, not twice) | −32 | — | — |
| **= real content** | **27,573,513** | **26.30 MB** | **1.89%** |
| alignment gaps inside the used region | 71 | — | ~0% |
| last byte used | 27,573,584 (0x1A4BD50) | 26.30 MB | |
| **trailing padding** | **1,432,404,656** | **1366.05 MB** | **98.11%** |

Totals are an **interval union**, not a sum: the apploader's documented extent
(`0x20` header + `size` + `trailer_size` = 113,508 B, ending at 0x1DFA4) runs
32 bytes past where `main.dol` actually begins on this master (0x1DF84), so
naive summing over-reports by 32. The 71 bytes of gaps are 32 B before the
FST, 34 B before `audiorom.img`, and 1+3+1 B of alignment slack between
files — the payload region is otherwise packed solid.

**The padding was verified, not assumed:** every one of the 1,432,404,656
bytes after the last file (offset 27,573,584 to EOF) was scanned and every one
of them is `0x00` — zero non-zero bytes in the whole 1.43 GB tail. The
project lead's belief is confirmed — the real content is ~26 MB, and a 700 MB
CD-R is *extremely* comfortable, not tight (see sizing below).

### Compressed size — the lead's "~16 MB" recollection was right

Payload = `main.dol` + all 10 FST files, concatenated (27,450,499 bytes raw;
excludes the GC system area the Dreamcast has no use for).

| codec | bytes | | ratio |
|---|---:|---:|---:|
| raw | 27,450,499 | 26.18 MB | 100% |
| **deflate -9 (zlib)** | **16,743,158** | **15.97 MB** | **60.99%** |
| lzma preset 6 | 14,668,548 | 13.99 MB | 53.44% |

15.97 MB at deflate -9 — the ~16 MB figure is accurate to two significant
figures. It is *not* the disc budget though; see the sizing table.

### File-size breakdown (all 10 FST files live at the disc root)

| file | offset | size | | share | magic | note |
|---|---|---:|---:|---:|---|---|
| `audiorom.img` | 0x000FE588 | 8,300,384 | 7.92 MB | 31.28% | `D3 20 D5 78` | jaudio bank; §3.4 target |
| `foresta.rel.szs` | 0x00F27B34 | 6,137,393 | 5.85 MB | 23.13% | `Yaz0` | → **15,640,056** decompressed |
| `foresta.map` | 0x00A87D3C | 4,849,144 | 4.62 MB | 18.28% | text | linker map (debug text) |
| `forest_2nd.arc` | 0x015D2508 | 4,132,608 | 3.94 MB | 15.58% | `RARC` | graph-ARAM archive |
| `famicom.arc` | 0x008E8CFC | 1,699,904 | 1.62 MB | 6.41% | `RARC` | NES ROMs — **non-goal, droppable** |
| `forest_1st.arc` | 0x01502168 | 852,896 | 0.81 MB | 3.21% | `RARC` | graph-ARAM archive |
| `static.map` | 0x019C4D68 | 552,879 | 0.53 MB | 2.08% | text | linker map (debug text) |
| `opening.bnr` | 0x019C3408 | 6,496 | 0.01 MB | 0.02% | `BNR1` | GC banner — unused on DC |
| `COPYDATE` | 0x008E8CE8 | 19 | — | ~0% | `02/0` | build date string |
| `static.str` | 0x01A4BD18 | 56 | — | ~0% | text | build path string |

By extension: `.img` 7.92 MB (31.28%) · `.arc` 6.38 MB (25.20%, 3 files) ·
`.rel.szs` 5.85 MB (23.13%) · `.map` 5.15 MB (20.36%, 2 files) · everything
else < 0.03%.
By directory: everything is in the root — the FST has **11 entries, 1
directory (root), 10 files, no subdirectories**. `pc_disc.c`'s
`MAX_FST_FILES 1024` / depth-32 limits are therefore never near the edge for
GAFE01, but this tool has no such limits.

**Audio share: `audiorom.img` is 8,300,384 bytes = 31.28% of FST content,
30.10% of real content** — the single largest file on the disc and the
largest single lever in PLAN §3.4.

Per-file deflate -9 (raw → compressed): `foresta.map` 15.7% ·
`static.map` 18.4% · `forest_1st.arc` 21.6% · `forest_2nd.arc` 34.7% ·
`main.dol` 43.5% · `audiorom.img` 82.4% · `foresta.rel.szs` 89.1% ·
`famicom.arc` 90.8%. The `.map` files are debug text and nearly free to
compress; `audiorom.img` and the already-Yaz0 REL are near-incompressible.

### Dreamcast disc sizing

| | bytes | | of a 700 MB CD-R |
|---|---:|---:|---:|
| payload, `foresta.rel` left Yaz0-compressed | 27,450,499 | 26.18 MB | 3.9% |
| **payload, `foresta.rel` expanded (default)** | **36,953,162** | **35.24 MB** | **5.3%** |
| CD-R capacity | 700,000,000 | 667.57 MB | 100% |

**Verdict: comfortable, with ~632 MB of headroom.** Disc space is a non-issue.
That headroom is what pays for the offline conversions that trade size for
CPU: pre-VQ'd/twiddled textures, decoded/AICA-ADPCM audio, and pre-split
per-asset blobs. Spend it freely.

---

## What the tool does today (all verified against the real image)

* Opens **ISO / GCM** and **CISO** (CISO block map re-implemented from
  `pc_disc.c`; verified by synthesizing a 2 MiB-block CISO of the real image
  and confirming **byte-identical extraction**, all 11 SHA-256s equal).
* **Validates** the image: GameCube magic `0xC2339F3D` at 0x1C, then
  `GAFE01` / disc 0 / rev 0. Refuses anything else without `--force`, because
  every downstream asset offset assumes this exact master.
* Parses the **boot header** (game id, name, DOL/FST offsets, user area), the
  **apploader** size, and walks the **FST** with a proper directory stack
  (arbitrary depth and file count, unlike the runtime version).
* Computes **`main.dol` size** from the DOL section table (7 text + 11 data
  sections, highest section end) = 918,720 bytes — same math as `pc_disc.c`.
* **Yaz0 decompression** for `foresta.rel.szs` → 15,640,056 bytes.
* **`report`** — the measurement above, plus `--json` for machine consumption.
* **`extract`** — the disc tree below, plus `manifest.json`
  (path, size, SHA-256, source offset, note) for downstream steps and the
  harness.
* **`verify`** — re-hashes an extracted tree against its manifest.
* **`relmap`** — see "Findings that change the plan" below.

### Yaz0 correctness

`tools/pyjkernel/jkrcomp.py` raises `NotImplementedError` for SZS, so no
independent decoder was available in-repo for a differential test. Instead the
output was **structurally validated as a REL/OSModule**: id 1, version 2,
20 sections, all section `offset+size` inside the file, highest section end
`0xDBC838` **exactly equal** to `relOffset`, and `impOffset + impSize`
(`0xEEA5E8 + 16`) **exactly equal** to the 15,640,056-byte file length. Three
independent header fields landing exactly on the file boundaries is a strong
correctness signal, but note it is a structural check, not a byte-for-byte
comparison against a reference decoder.

### Extract output

```
$OUT/
  sys/main.dol            918,720     asset blob for pc_assets.c SRC_DOL
  files/audiorom.img    8,300,384
  files/COPYDATE               19
  files/famicom.arc     1,699,904
  files/foresta.map     4,849,144
  files/foresta.rel    15,640,056     Yaz0-expanded from foresta.rel.szs
  files/forest_1st.arc    852,896
  files/forest_2nd.arc  4,132,608
  files/opening.bnr         6,496
  files/static.map        552,879
  files/static.str             56
  manifest.json          11 entries, 36,953,162 bytes total
```

`files/` deliberately mirrors the `assets/` fallback layout `pc_dvd.c` already
understands (`DVDOpen("COPYDATE")` → `assets/COPYDATE`), so the DC `DVDOpen`
implementation is a path rewrite to `/cd/files/…` and nothing more.

`--keep-sys` additionally emits `sys/boot.bin`, `sys/bi2.bin`,
`sys/apploader.img`, `sys/fst.bin` for reference/reconstruction. None are
needed on a Dreamcast disc.

Reference hashes for GAFE01 rev 0 (SHA-256, first 16 hex chars):

| path | size | sha256[:16] |
|---|---:|---|
| `sys/main.dol` | 918,720 | `e3166b15b810ff20` |
| `files/audiorom.img` | 8,300,384 | `3a631dac1a2abb0d` |
| `files/COPYDATE` | 19 | `1f96d66aa55300c4` |
| `files/famicom.arc` | 1,699,904 | `3ac09f56fcd3d6cb` |
| `files/foresta.map` | 4,849,144 | `4fd11c8a63aa5123` |
| `files/foresta.rel` | 15,640,056 | `29306a927eee861b` |
| `files/forest_1st.arc` | 852,896 | `461d3d0e0293dac2` |
| `files/forest_2nd.arc` | 4,132,608 | `b3d784fbb993e83f` |
| `files/opening.bnr` | 6,496 | `63178c806a40e1b7` |
| `files/static.map` | 552,879 | `9de085eeceec11a0` |
| `files/static.str` | 56 | `e4b65b9e510a82a2` |

---

## Findings that change the plan

**1. The 16.6 MB REL blob is unnecessary — and `pack` now removes it.**
`pc_assets.c` holds the *entire* decompressed REL (15,640,056 B) plus
`main.dol` (918,720 B) in RAM for the whole of `pc_assets_init()` and
`memcpy`s assets out of them by absolute offset (`pc_load_asset`), then frees
both. That is a 16.56 MB peak — more than the Dreamcast's entire RAM.
`dcasset relmap` parses every `pc_load_asset` reference — the 14,495-entry
`s_assets[]` table plus the generated call sites in `src/` — and merges the
byte ranges:

| blob | refs | raw bytes | **merged union** | spans | max end | blob size |
|---|---:|---:|---:|---:|---|---:|
| `foresta.rel` | 16,306 | 8,762,128 | **8,664,560** (8.26 MB) | 2,496 | 0xDBC720 | 15,640,056 |
| `main.dol` | 59 | 122,766 | **122,702** (0.12 MB) | 13 | 0xC9A40 | 918,720 |

Coverage is exact, not sampled: of 2,641 textual `pc_load_asset` occurrences
under `src/`, 770 are prototypes and 1,871 are parsed call sites — **0
unaccounted** (relmap prints this reconciliation and warns if it is ever
non-zero). One call site in `JFWSystem.cpp` was initially missed because the
generated code puts `/* SRC_DOL */` comments between arguments; the regexes
now tolerate that.

Only **8.66 MB of the 15.64 MB REL is ever read**, in 2,496 disjoint spans,
and nothing above `0xDBC720`. Two consequences:
* The bytes above `0xDBC720` (relocations at 0xDBC838 + imports at 0xEEA5E8,
  ~1.24 MB, plus unreferenced data) never need to reach the Dreamcast at all.
* Better still, the blobs need not be resident: emit the 2,496 + 13 spans as a
  packed, offset-indexed asset file and have `pc_load_asset` `fread` from
  `/cd` instead of `memcpy`. Peak RAM for asset loading drops from 16.56 MB to
  one read buffer. **This is the single biggest RAM win the extractor can
  hand PLAN §3.1**, and it is a host-side change (`gen_runtime_assets.py`
  extension) plus a small change in `pc_load_asset`.

The earlier caveat — "only `pc_load_asset` sites were parsed, so this is a
lower bound" — was **audited and closed** while building `pack`:
`g_rel_data` / `g_dol_data` are `static` in `pc_assets.c` and referenced only
inside `pc_load_asset`; `pc_disc_extract_rel/dol()` have exactly one caller
each; and `boot.c` compiles `LoadLink("/foresta.rel.szs")` out under
`TARGET_PC`. 8,787,262 distinct bytes is the **complete** footprint. Details
and evidence in `kb/asset-pack.md` §7.

**2. `famicom.arc` (1,699,904 B) is droppable.** The NES emulator is an
explicit non-goal (PLAN §4). Removing it is 6.4% of FST content.

**3. `foresta.map` + `static.map` (5,402,023 B, 20.4% of FST content) are
droppable — verified.** They are read only by
`JUTException::queryMapAddress_single()`, reachable only from the PowerPC
CPU-exception handler, and `showMapInfo_subroutine()` bails on any address
outside `0x80000000..0x82FFFFFF` (a GameCube MEM1 window that no SH-4 address
satisfies). Full evidence chain in `kb/asset-pack.md` §8. `dvderr.c` was
checked specifically and contains no map reference.

**4. The pack: `assets.pak`, 8,917,568 B.** `dcasset pack` emits the 3,188
chunks the game actually reads, pre-byte-swapped, laid out in
`pc_assets_init`'s real call order so the Dreamcast streams them front to back
with **zero backward seeks** given an 8 KB window. Resident cost drops from
16,558,776 B to a 51,104 B index (+ a 64 KB read buffer) — **15.68 MB saved,
98.0% of the machine.** It also replaces the 15,640,056-byte `foresta.rel` on
the disc, so the DC needs no Yaz0 decompressor at boot. Byte-exactness is
re-proved every run by replaying all 16,365 references through the runtime
lookup algorithm (8,884,894 B compared, 0 mismatches). Format, lookup
algorithm, read-ahead strategy and caveats: **`kb/asset-pack.md`**.

**5. The next RAM problem is the destinations, not the source.** The assets
land in 15,726 statically allocated arrays whose extents sum to ~8,617,214 B
(8.22 MB), resident for the whole run regardless of where the bytes came
from. The pack removes the 15.68 MB peak; it does not touch that 8.22 MB.
`kb/mem-budget.md` needs it as its own line.

---

## Usage

Pure Python 3.9+ stdlib — nothing to compile. C was considered (reusing
`pc/src/pc_disc.c` directly) and rejected: that file is built around file-
static globals, `TARGET_PC`, `types.h`, a 1024-file cap and a cwd-scanning
`find_disc_image()`, so making it a host tool means rewriting it anyway; while
the parts a *host* tool needs — SHA-256, deflate/lzma measurement, JSON
manifests, tree emission — are all stdlib in Python and hand-rolled in C. The
repo's host tools are already Python (`pc/tools/gen_runtime_assets.py`,
`tools/texture_tool.py`, `tools/arc_tool.py`, `tools/msg_tool.py`) with a
`.flake8` config, and the remaining pipeline stages (texture VQ, ADPCM) will
be Python too. The ~150 lines of parsing logic from `pc_disc.c` are
re-implemented faithfully in `gcm.py`, which cites it. `make lint` passes.

```bash
cd tools/dcasset

make report                            # the measurement above
make report-json                       # + /tmp/opencrossing-dc/report.json
make extract                           # -> /tmp/opencrossing-dc/discroot
make verify                            # re-hash against manifest.json
make relmap                            # REL/DOL span analysis
make pack                              # -> discroot/files/assets.pak
make packverify                        # independent re-verification
make all                               # report + extract + pack + verify + packverify

# override anything
make extract ISO=/path/to/other.gcm OUT=/tmp/other-root

# or call it directly
python3 dcasset.py report     "$ISO" [--json F] [--no-lzma] [--force]
python3 dcasset.py extract    "$ISO" [--out DIR] [--keep-szs] [--keep-sys] [--force]
python3 dcasset.py verify     DIR
python3 dcasset.py relmap     [--repo DIR] [--json F] [--spans]
python3 dcasset.py pack       "$ISO" [--out DIR] [--repo DIR]
                              [--order load|first-touch|source] [--align N] [--quick]
python3 dcasset.py packverify PAK "$ISO" [--repo DIR]
```

Timings on the M4 host: `report` 12–20 s (dominated by the lzma pass; ~2 s
with `--no-lzma`), `extract` 1.0 s including Yaz0 and all SHA-256s, `relmap`
0.2 s, `pack` ~3 s including Yaz0, full 16,365-reference round trip and both
comparison layouts.

### Files

| file | role |
|---|---|
| `dcasset.py` | CLI: `report` / `extract` / `verify` / `relmap` / `pack` / `packverify` |
| `gcm.py` | ISO/GCM/CISO reader, FST walk, DOL sizing, Yaz0 — read-only |
| `assets_scan.py` | parses `pc_assets.c` into the **ordered** reference list; shared by `relmap` and `pack` so they cannot disagree |
| `pack.py` | chunking, offline byte-swap, layout, index, verification, read-ahead simulation |

Then:

```bash
mkdcdisc -D /tmp/opencrossing-dc/discroot -e 1ST_READ.BIN -o OpenCrossing.cdi
```

**The resulting CDI contains Nintendo assets and is never distributed.** The
repo ships tools only (dca3 / sm64-dc legal model, `kb/research-ecosystem.md`
§9).

---

## What remains — for later agents

Conversion, in rough dependency order. None of it is implemented here; today
the tool only *extracts and measures*.

1. ~~**Per-asset REL/DOL splitting.**~~ **Done** — `dcasset pack`, spec in
   `kb/asset-pack.md`. The offline endian swap (`SWAP_U16` / `SWAP_VTX` /
   `SWAP_U32`) is baked into the pack too, so the SH-4 never pays for it.
   What remains is the `dc/` side: implement §5's lookup and §6's read-ahead
   in `pc_load_asset`, and measure the four *other* boot-time swap passes
   (`pc_bswap_raw_display_lists`, `mFM_InitActableEndian`,
   `pc_bswap_u8_tlut_palettes`, `pc_bswap_house_pos_list`) which operate on
   the destinations and are not covered by the pack.
2. **Textures → PVR** (PLAN §3.3). GC CI4/CI8 → PVR native 4/8-bit paletted;
   RGB565/RGB5A3/IA → twiddled 16-bit; the rest → VQ (offline only; `pvrtex`
   from KOS utils). Needs the harvested-texture corpus from a playthrough,
   same content-hash keying the base port's texture cache already uses.
   `forest_1st.arc` / `forest_2nd.arc` / `famicom.arc` are RARC — use
   `tools/arc_tool.py` + `tools/pyjkernel` to walk them; note
   `pyjkernel.jkrcomp.decompress_szs` is unimplemented, so any Yaz0 members
   inside RARCs need `gcm.yaz0_decode` from this package.
3. **Audio → AICA** (PLAN §3.4). Split the 8.3 MB `audiorom.img`: instrument
   samples → AICA ADPCM in the 2 MB sound RAM (or disc-streamed), sequence
   data → main RAM. Stage A (rspsim on SH-4) needs no conversion at all, so
   this is gated on the stage-A measurement.
4. **Drop what the DC does not need:** `famicom.arc` (confirmed non-goal),
   `opening.bnr` (GC banner), the GC system area, `foresta.map` +
   `static.map` (audit done, see finding 3), and `foresta.rel` itself once
   `assets.pak` is wired up (finding 4). That removes 22,748,479 B and adds
   the pack's 8,917,568 B, taking the recommended layout from 36,953,162 B
   (35.24 MB) to **23,122,251 B (22.05 MB)** — 3.3% of a CD-R.
5. **Disc layout ordering.** `mkdcdisc` places files in tree order; CD-R
   streams at ~500 KB/s, so hot files belong on outer tracks. The manifest
   should grow a `stream_priority` field once profiling says which files are
   hot. Not measurable yet.
6. **Harness hook.** `harness/dc/smoke.sh` should call `dcasset verify` before
   building a CDI so a truncated or stale extraction fails loudly rather than
   as a mystery crash on boot.
