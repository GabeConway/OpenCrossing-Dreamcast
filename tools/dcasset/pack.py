"""Build the Dreamcast offset-indexed asset pack (``dcasset pack``).

Why this exists
---------------
``pc_assets_init()`` decompresses the whole 15,640,056-byte ``foresta.rel``
and loads the whole 918,720-byte ``main.dol`` into RAM, ``memcpy``s ~8.8 MB of
assets out of them by absolute offset, then ``free()``s both. Peak allocation
is **16,558,776 bytes of blob** on a machine with 16,777,216 bytes of RAM
total. It cannot run on a stock Dreamcast.

``dcasset relmap`` proved only 8,664,560 + 122,702 bytes of those blobs are
ever touched. This module emits exactly those bytes as one file the Dreamcast
streams off ``/cd``, so ``pc_load_asset`` becomes read-at-offset into the
caller's destination and the blob disappears.

Design decisions and their justification
----------------------------------------
* **Little-endian ILP32 fields.** SH-4 reads the index with no conversion.

* **Byte swap baked in offline.** ``pc_load_asset``'s ``do_swap`` runs over
  8.8 MB at boot on a 200 MHz SH-4. Every chunk whose swap recipe is
  unambiguous is written pre-swapped and flagged, and the runtime skips the
  swap entirely. Chunks where two overlapping references disagree about the
  swap are written raw and flagged so the runtime still swaps them --
  correctness first, and the tool reports how many there are.

* **Payload ordered by first touch at *load-order chunk* granularity.**
  The boot-time asset load is a statically known sequence: ``s_assets[]`` in
  table order, then 769 ``_pc_load_src_*()`` functions in ``pc_assets_init``'s
  textual order, each in its own textual order. Grouping consecutive
  references that are also contiguous in the blob yields 3,188 chunks that are
  read strictly front-to-back. A CD-R streams at ~500 KB/s but a seek costs
  10-300 ms, so eliminating seeks matters far more than eliminating bytes:
  the alternative layouts (merged spans in first-touch or source order) cost
  3,302 and 13,979 *backward* seeks respectively. The chunk layout duplicates
  64,640 bytes (0.7%) of blob content to buy that, which is free on a disc
  that is 5% full. ``--order first-touch`` / ``--order source`` build the
  merged-span layouts for comparison; the tool measures all three.

* **32-byte chunk alignment.** Matches the SH-4 cache line / store-queue
  granularity, and costs ~19 KB of disc. Sector (2048 B) alignment was
  rejected: it would inflate the payload by ~3 MB, i.e. ~6 s of extra boot
  streaming at 500 KB/s, to buy nothing the sector cache does not already.
"""

from __future__ import annotations

import bisect
import json
import struct
import time
import zlib
from array import array
from dataclasses import dataclass, field
from pathlib import Path

import assets_scan as A

PACK_MAGIC = b"DCASSET1"
PACK_VERSION = 1
HEADER_SIZE = 64
SOURCE_ENTRY_SIZE = 16
INDEX_ENTRY_SIZE = 12
SORTED_ENTRY_SIZE = 4
DEFAULT_ALIGN = 32
DEFAULT_READ_BUFFER = 64 * 1024

LEN_BITS = 28
LEN_MASK = (1 << LEN_BITS) - 1
SRC_SHIFT = 28
SRC_MASK = 0x3
FLAG_PRESWAPPED = 1 << 31

# Status-quo peak this pack removes (bytes).
STATUS_QUO_BLOB = A.REL_BLOB_SIZE + A.DOL_BLOB_SIZE   # 16,558,776
DC_RAM = 16 * 1024 * 1024


# ------------------------------------------------------------ byte swapping
# Byte-exact mirrors of pc_bswap_asset_* in pc/src/pc_assets.c, including
# their truncating element counts (size/2, size/4, size/16): a trailing
# partial element is left untouched there, so it is left untouched here.


def swap_u16(b: bytes) -> bytes:
    n = (len(b) // 2) * 2
    a = array('H')
    a.frombytes(b[:n])
    a.byteswap()
    return a.tobytes() + b[n:]


def swap_u32(b: bytes) -> bytes:
    n = (len(b) // 4) * 4
    a = array('I')
    a.frombytes(b[:n])
    a.byteswap()
    return a.tobytes() + b[n:]


def swap_vtx(b: bytes) -> bytes:
    """16-byte Vtx records: bytes 0..11 swapped as u16 pairs, 12..15 kept."""
    n = (len(b) // 16) * 16
    head = b[:n]
    a = array('H')
    a.frombytes(head)
    a.byteswap()
    out = bytearray(a.tobytes())
    for k in range(12, 16):        # restore the untouched tail of each record
        out[k::16] = head[k::16]
    return bytes(out) + b[n:]


_SWAPPERS = {
    A.SWAP_NONE: lambda b: b,
    A.SWAP_U16: swap_u16,
    A.SWAP_VTX: swap_vtx,
    A.SWAP_U32: swap_u32,
}


def apply_swap(b: bytes, swap: int) -> bytes:
    return _SWAPPERS[swap](b)


# ------------------------------------------------------------------- chunks


@dataclass
class Chunk:
    src: int
    start: int
    end: int
    order: int                     # position in the payload
    refs: list = field(default_factory=list)
    data: bytes = b""
    preswapped: bool = True
    pack_off: int = 0
    conflict: dict | None = None

    @property
    def size(self) -> int:
        return self.end - self.start


def chunks_load_order(refs: list[A.AssetRef]) -> list[Chunk]:
    """Group consecutive references that are also contiguous in the blob.

    A reference joins the open chunk when it belongs to the same source and
    starts at or before the chunk's current end (so no hole is created) --
    which covers the overwhelmingly common generated pattern of one asset
    immediately following the previous one. Anything else opens a new chunk.
    Chunks may therefore overlap each other in *source* space; that costs a
    few duplicated bytes and is what buys a seek-free read.
    """
    out: list[Chunk] = []
    cur: Chunk | None = None
    for r in refs:
        if (cur is not None and cur.src == r.src
                and cur.start <= r.off <= cur.end):
            cur.end = max(cur.end, r.end)
            cur.refs.append(r)
            continue
        cur = Chunk(r.src, r.off, r.end, len(out), [r])
        out.append(cur)
    return out


def chunks_merged(refs: list[A.AssetRef], order: str) -> list[Chunk]:
    """Maximal disjoint spans, ordered by first touch or by source offset."""
    out: list[Chunk] = []
    for src in (A.SRC_REL, A.SRC_DOL):
        spans = A.merge_spans(refs, src)
        if not spans:
            continue
        starts = [a for a, _ in spans]
        made = [Chunk(src, a, b, 0) for a, b in spans]
        for r in (x for x in refs if x.src == src):
            i = bisect.bisect_right(starts, r.off) - 1
            made[i].refs.append(r)
        out += made
    if order == "first-touch":
        out.sort(key=lambda c: (min(r.seq for r in c.refs), c.src, c.start))
    else:
        out.sort(key=lambda c: (c.src, c.start))
    for i, c in enumerate(out):
        c.order = i
    return out


def bake(chunks: list[Chunk], blobs: dict[int, bytes]) -> None:
    """Apply each reference's byte swap into the chunk payload, offline.

    A chunk can only be pre-swapped if every byte has one agreed value. If two
    references disagree (different swap type, or the same type at a different
    alignment) the chunk is emitted raw and the runtime does the swap.
    """
    for c in chunks:
        raw = blobs[c.src][c.start:c.end]
        out = bytearray(raw)
        touched = bytearray(c.size)
        for r in c.refs:
            o, n = r.off - c.start, r.size
            sw = apply_swap(bytes(raw[o:o + n]), r.swap)
            seg = touched[o:o + n]
            if 1 in seg:
                cur = out[o:o + n]
                bad = next((k for k in range(n)
                            if seg[k] and cur[k] != sw[k]), None)
                if bad is not None:
                    c.conflict = {
                        "chunk": c.order, "source": A.SOURCE_NAMES[c.src],
                        "chunk_start": c.start, "chunk_end": c.end,
                        "byte": c.start + o + bad, "ref_off": r.off,
                        "ref_size": r.size, "ref_swap": A.SWAP_NAMES[r.swap],
                        "ref_origin": r.origin,
                    }
                    break
            out[o:o + n] = sw
            touched[o:o + n] = b"\x01" * n
        if c.conflict is not None:
            c.data = bytes(raw)
            c.preswapped = False
        else:
            if 0 in touched:
                raise AssertionError(
                    f"chunk {c.order} has bytes covered by no reference")
            c.data = bytes(out)
            c.preswapped = True


# ------------------------------------------------------------------ lookup
# Reference implementation of the algorithm the dc/ runtime must implement.
# Kept here so the round-trip test exercises the *runtime's* path, not the
# builder's bookkeeping.


class PackIndex:
    """Decoded index, mirroring what the Dreamcast keeps resident."""

    def __init__(self, image: bytes):
        (magic, ver, hdr_size, flags, nsrc, index_off, nentries,
         sorted_off, payload_off, payload_bytes, align, max_chunk,
         crc, rel_size, dol_size) = struct.unpack_from(
            "<8sIIIIIIIIIIIIII", image, 0)
        if magic != PACK_MAGIC:
            raise ValueError("bad magic")
        self.version = ver
        self.flags = flags
        self.align = align
        self.max_chunk = max_chunk
        self.payload_off = payload_off
        self.payload_bytes = payload_bytes
        self.crc = crc
        self.entries = []
        for i in range(nentries):
            so, po, lf = struct.unpack_from(
                "<III", image, index_off + i * INDEX_ENTRY_SIZE)
            self.entries.append((so, po, lf & LEN_MASK,
                                 (lf >> SRC_SHIFT) & SRC_MASK,
                                 bool(lf & FLAG_PRESWAPPED)))
        self.sorted = list(struct.unpack_from(
            f"<{nentries}I", image, sorted_off))
        self.keys = [(self.entries[k][3], self.entries[k][0])
                     for k in self.sorted]
        self.cursor = 0
        self.backscan_max = 0

    def _fits(self, i: int, src: int, off: int, size: int) -> bool:
        so, _po, ln, s, _ps = self.entries[i]
        return s == src and so <= off and off + size <= so + ln

    def find(self, src: int, off: int, size: int) -> int:
        """Cursor fast path, then binary search + bounded back-scan."""
        n = len(self.entries)
        for cand in (self.cursor, self.cursor + 1):
            if cand < n and self._fits(cand, src, off, size):
                self.cursor = cand
                return cand
        k = bisect.bisect_right(self.keys, (src, off)) - 1
        depth = 0
        while k >= 0 and self.keys[k][0] == src:
            i = self.sorted[k]
            if self._fits(i, src, off, size):
                self.backscan_max = max(self.backscan_max, depth)
                self.cursor = i
                return i
            k -= 1
            depth += 1
        raise KeyError(f"src {src} off 0x{off:X} size {size} not in pack")

    def read(self, image: bytes, src: int, off: int, size: int,
             swap: int) -> bytes:
        i = self.find(src, off, size)
        so, po, _ln, _s, ps = self.entries[i]
        data = image[po + (off - so):po + (off - so) + size]
        return data if ps else apply_swap(data, swap)


# ----------------------------------------------------------------- building


def build_pack(refs: list[A.AssetRef], blobs: dict[int, bytes],
               order: str = "load", align: int = DEFAULT_ALIGN) -> dict:
    if order == "load":
        chunks = chunks_load_order(refs)
    else:
        chunks = chunks_merged(refs, order)
    bake(chunks, blobs)

    sources = sorted({c.src for c in chunks})
    n = len(chunks)
    index_off = HEADER_SIZE + SOURCE_ENTRY_SIZE * len(sources)
    sorted_off = index_off + INDEX_ENTRY_SIZE * n
    payload_off = _align(sorted_off + SORTED_ENTRY_SIZE * n, align)

    body = bytearray()
    cursor = payload_off
    max_chunk = 0
    for c in chunks:
        pad = _align(cursor, align) - cursor
        body += b"\x00" * pad
        cursor += pad
        c.pack_off = cursor
        body += c.data
        cursor += len(c.data)
        max_chunk = max(max_chunk, len(c.data))

    index = bytearray()
    for c in chunks:
        lf = c.size | (c.src << SRC_SHIFT)
        if c.preswapped:
            lf |= FLAG_PRESWAPPED
        index += struct.pack("<III", c.start, c.pack_off, lf)

    order_sorted = sorted(range(n), key=lambda i: (chunks[i].src,
                                                   chunks[i].start,
                                                   chunks[i].end))
    sorted_tbl = struct.pack(f"<{n}I", *order_sorted)

    crc = zlib.crc32(bytes(body)) & 0xFFFFFFFF
    header = bytearray(HEADER_SIZE)
    struct.pack_into(
        "<8sIIIIIIIIIIIIII", header, 0,
        PACK_MAGIC, PACK_VERSION, HEADER_SIZE,
        1 if all(c.preswapped for c in chunks) else 0,
        len(sources), index_off, n, sorted_off, payload_off, len(body),
        align, max_chunk, crc, A.REL_BLOB_SIZE, A.DOL_BLOB_SIZE)

    src_table = bytearray()
    for s in sources:
        cs = [c for c in chunks if c.src == s]
        src_table += struct.pack("<IIII", s, len(cs), A.BLOB_SIZE[s],
                                 max(c.end for c in cs))

    image = bytearray()
    image += header
    image += src_table
    image += index
    image += sorted_tbl
    image += b"\x00" * (payload_off - len(image))
    image += body

    chunk_bytes = sum(c.size for c in chunks)
    stats = {
        "order": order,
        "align": align,
        "header_size": HEADER_SIZE,
        "source_table_bytes": len(src_table),
        "index_offset": index_off,
        "index_entries": n,
        "index_bytes": len(index),
        "sorted_table_offset": sorted_off,
        "sorted_table_bytes": len(sorted_tbl),
        "resident_index_bytes": (HEADER_SIZE + len(src_table) + len(index)
                                 + len(sorted_tbl)),
        "payload_offset": payload_off,
        "payload_bytes": len(body),
        "chunk_bytes": chunk_bytes,
        "payload_alignment_padding": len(body) - chunk_bytes,
        "file_bytes": len(image),
        "max_chunk_bytes": max_chunk,
        "payload_crc32": crc,
        "chunks_preswapped": sum(1 for c in chunks if c.preswapped),
        "chunks_raw_due_to_swap_conflict": sum(
            1 for c in chunks if not c.preswapped),
        "swap_conflicts": [c.conflict for c in chunks if c.conflict][:20],
        "sources": {},
    }
    for s in sources:
        cs = [c for c in chunks if c.src == s]
        rs = [r for r in refs if r.src == s]
        union = sum(b - a for a, b in A.merge_spans(refs, s))
        stats["sources"][A.SOURCE_NAMES[s]] = {
            "source_id": s,
            "blob_size": A.BLOB_SIZE[s],
            "references": len(rs),
            "reference_bytes": sum(r.size for r in rs),
            "unique_bytes": union,
            "chunks": len(cs),
            "chunk_bytes": sum(c.size for c in cs),
            "duplicated_bytes": sum(c.size for c in cs) - union,
            "max_referenced_offset": max(c.end for c in cs),
            "chunks_preswapped": sum(1 for c in cs if c.preswapped),
        }
    return {"bytes": bytes(image), "stats": stats, "chunks": chunks}


def _align(x: int, a: int) -> int:
    return (x + a - 1) // a * a


# ------------------------------------------------------------ verification


def roundtrip(image: bytes, refs: list[A.AssetRef],
              blobs: dict[int, bytes]) -> dict:
    """Replay every reference through the runtime lookup and compare bytes.

    ``want`` is what ``pc_load_asset`` on PC would have left in the caller's
    destination: ``memcpy`` from the blob, then ``do_swap``. If that matches
    for all 16,365 references, the pack is a drop-in replacement for the
    resident blob.
    """
    idx = PackIndex(image)
    bad = []
    checked = 0
    nbytes = 0
    for r in refs:
        got = idx.read(image, r.src, r.off, r.size, r.swap)
        want = apply_swap(blobs[r.src][r.off:r.end], r.swap)
        if got != want:
            if len(bad) < 20:
                bad.append({"off": r.off, "size": r.size,
                            "source": A.SOURCE_NAMES[r.src],
                            "swap": A.SWAP_NAMES[r.swap],
                            "origin": r.origin})
        checked += 1
        nbytes += r.size
    payload = image[idx.payload_off:idx.payload_off + idx.payload_bytes]
    crc_ok = (zlib.crc32(payload) & 0xFFFFFFFF) == idx.crc
    return {
        "references_replayed": checked,
        "bytes_compared": nbytes,
        "mismatches": len(bad),
        "mismatch_examples": bad,
        "payload_crc32_ok": crc_ok,
        "max_backscan_depth": idx.backscan_max,
        "ok": not bad and crc_ok,
    }


def swap_selftest(rng_seed: int = 20260801) -> dict:
    """Check the array-based swappers against literal ports of the C loops.

    pack.py's swappers use array.byteswap() and extended slices for speed;
    the C in pc_assets.c is three plain loops. If they ever disagree the pack
    is silently wrong for every asset of that class, so they are compared
    element-by-element on random data at every size class.
    """
    import random
    rnd = random.Random(rng_seed)

    def c_u16(b):
        p = bytearray(b)
        for i in range(len(b) // 2):
            p[i * 2], p[i * 2 + 1] = p[i * 2 + 1], p[i * 2]
        return bytes(p)

    def c_u32(b):
        p = bytearray(b)
        for i in range(len(b) // 4):
            q = i * 4
            p[q:q + 4] = p[q:q + 4][::-1]
        return bytes(p)

    def c_vtx(b):
        p = bytearray(b)
        for i in range(len(b) // 16):
            q = i * 16
            for j in range(0, 12, 2):
                p[q + j], p[q + j + 1] = p[q + j + 1], p[q + j]
        return bytes(p)

    cases = 0
    for size in (0, 1, 2, 3, 4, 5, 15, 16, 17, 31, 32, 33, 64, 255, 4096):
        data = bytes(rnd.randrange(256) for _ in range(size))
        for fn, ref, name in ((swap_u16, c_u16, "u16"),
                              (swap_u32, c_u32, "u32"),
                              (swap_vtx, c_vtx, "vtx")):
            if fn(data) != ref(data):
                raise AssertionError(f"swap {name} mismatch at size {size}")
            cases += 1
    return {"cases": cases, "ok": True}


def readahead_sim(image: bytes, refs: list[A.AssetRef],
                  buf_sizes=(2048, 4096, 8192, 16384, 32768, 65536)) -> dict:
    """Simulate a forward-only read-ahead window and count seeks.

    Model, matching what dc/ will implement on KOS ``fs_read``:
      * one open handle on ``/cd/files/assets.pak``, never rewound;
      * a sliding window holding the last ``B`` bytes that were read;
      * a request inside the window is a ``memcpy``, free;
      * a request past the window streams forward -- contiguous, no seek;
      * a request starting *before* ``head - B`` is a **window fault**: the
        only case that forces a real backward seek, at 10-300 ms each.
    With the load-order layout the largest backward reach is 7,520 bytes, so
    any window of 8 KB or more takes the fault count to zero and the entire
    boot-time asset load becomes one linear read of the file.
    """
    idx = PackIndex(image)
    plan = []
    for r in refs:
        i = idx.find(r.src, r.off, r.size)
        so, po, _ln, _s, _ps = idx.entries[i]
        plan.append((po + (r.off - so), r.size))

    out = {}
    for B in buf_sizes:
        head = 0
        faults = 0
        fault_bytes = 0
        served = 0
        streamed = 0
        for pos, size in plan:
            if size == 0:
                continue
            if pos < head - B:
                faults += 1
                fault_bytes += size
                continue
            if pos + size <= head:
                served += 1
                continue
            streamed += (pos + size) - head
            head = pos + size
        out[str(B)] = {
            "buffer_bytes": B,
            "window_faults": faults,
            "window_fault_bytes": fault_bytes,
            "reads_served_from_window": served,
            "bytes_streamed": streamed,
            "overhead_vs_file": round(streamed / len(image), 4),
            "stream_seconds_at_500kb_s": round(streamed / 500000.0, 1),
        }
    return out


def access_profile(image: bytes, refs: list[A.AssetRef]) -> dict:
    """Measure how sequential the boot-time read of this pack actually is.

    A CD-R streams at ~500 KB/s but a seek costs 10-300 ms, so the number that
    decides the layout is not "how many bytes" but "how often does the loader
    go backwards". ``min_sliding_window_bytes`` is the smallest forward-only
    buffer that would have served every read with zero re-seeks.
    """
    idx = PackIndex(image)
    fwd = back = 0
    max_back = 0
    high = 0
    window = 0
    lookups_fast = 0
    for r in refs:
        prev = idx.cursor
        i = idx.find(r.src, r.off, r.size)
        if i in (prev, prev + 1):
            lookups_fast += 1
        so, po, _ln, _s, _ps = idx.entries[i]
        pos = po + (r.off - so)
        if pos >= high:
            fwd += 1
        else:
            back += 1
            max_back = max(max_back, high - pos)
        window = max(window, high - pos)
        high = max(high, pos + r.size)
    return {
        "reads": len(refs),
        "forward_reads": fwd,
        "backward_reads": back,
        "backward_fraction": round(back / max(1, len(refs)), 6),
        "max_backward_bytes": max_back,
        "min_sliding_window_bytes": max(0, window),
        "lookups_via_cursor_fast_path": lookups_fast,
        "lookups_via_binary_search": len(refs) - lookups_fast,
        "high_water_bytes": high,
    }


# -------------------------------------------------------------------- entry


def run(image_path: str, repo: Path, out_dir: Path, order: str,
        align: int, quick: bool, gcm_mod, mib, commas) -> dict:
    t0 = time.time()
    refs, scan_stats = A.scan(repo)
    if scan_stats["src_unaccounted"]:
        raise SystemExit("refusing to pack: unaccounted pc_load_asset call "
                         "sites -- the span set would be a lower bound")

    reader, info = gcm_mod.parse(image_path)
    try:
        dol = reader.read(info.dol_offset, info.dol_size)
        rel_e = next(f for f in info.files if f.path == "foresta.rel.szs")
        rel = gcm_mod.yaz0_decode(reader.read(rel_e.offset, rel_e.size))
    finally:
        reader.close()
    if len(rel) != A.REL_BLOB_SIZE or len(dol) != A.DOL_BLOB_SIZE:
        raise SystemExit(f"blob size mismatch: rel={len(rel)} dol={len(dol)}")
    blobs = {A.SRC_REL: rel, A.SRC_DOL: dol}
    t_extract = time.time()

    built = build_pack(refs, blobs, order=order, align=align)
    image = built["bytes"]
    t_build = time.time()

    selftest = swap_selftest()
    rt = roundtrip(image, refs, blobs)
    prof = access_profile(image, refs)
    ra = readahead_sim(image, refs)
    alt = {}
    if not quick:
        for other in ("load", "first-touch", "source"):
            if other == order:
                continue
            b2 = build_pack(refs, blobs, order=other, align=align)
            p2 = access_profile(b2["bytes"], refs)
            p2["file_bytes"] = b2["stats"]["file_bytes"]
            p2["index_entries"] = b2["stats"]["index_entries"]
            p2["resident_index_bytes"] = b2["stats"]["resident_index_bytes"]
            alt[other] = p2
    t_verify = time.time()

    out_dir.mkdir(parents=True, exist_ok=True)
    pak = out_dir / "assets.pak"
    pak.write_bytes(image)

    st = built["stats"]
    resident = st["resident_index_bytes"]
    peak = resident + DEFAULT_READ_BUFFER
    min_buf = min((v["buffer_bytes"] for v in ra.values()
                   if v["window_faults"] == 0), default=None)
    manifest = {
        "manifest_version": 1,
        "tool": "tools/dcasset/dcasset.py pack",
        "generated_unix": int(time.time()),
        "pack_file": str(pak),
        "format": {
            "magic": PACK_MAGIC.decode(),
            "version": PACK_VERSION,
            "endian": "little",
            "header_size": HEADER_SIZE,
            "source_entry_size": SOURCE_ENTRY_SIZE,
            "index_entry_size": INDEX_ENTRY_SIZE,
            "sorted_entry_size": SORTED_ENTRY_SIZE,
            "chunk_alignment": align,
            "spec": "kb/asset-pack.md",
        },
        "source_image": {
            "path": str(Path(image_path).resolve()),
            "game_id": info.game_id,
            "disc_number": info.disc_number,
            "version": info.version,
            "file_size": info.file_size,
        },
        "scan": scan_stats,
        "pack": st,
        "totals": {
            "references": len(refs),
            "reference_bytes": sum(r.size for r in refs),
            "unique_referenced_bytes": sum(
                v["unique_bytes"] for v in st["sources"].values()),
            "chunks": st["index_entries"],
            "chunk_bytes": st["chunk_bytes"],
            "duplicated_bytes": sum(
                v["duplicated_bytes"] for v in st["sources"].values()),
        },
        "ram": {
            "status_quo_peak_blob_bytes": STATUS_QUO_BLOB,
            "status_quo_note": (
                "pc_assets_init() holds foresta.rel (15,640,056) and main.dol "
                "(918,720) simultaneously for the whole asset load and "
                "free()s both at the end. It is a boot-time peak, not a "
                "steady-state resident cost -- but the peak is what a "
                "16,777,216-byte machine cannot pay, and it is on top of the "
                "~8.8 MB of static destination arrays that stay resident "
                "either way."),
            "pack_resident_index_bytes": resident,
            "pack_read_buffer_bytes_minimum_measured": min_buf,
            "pack_read_buffer_bytes_recommended": DEFAULT_READ_BUFFER,
            "pack_peak_bytes": peak,
            "saving_bytes": STATUS_QUO_BLOB - peak,
            "saving_mb": round((STATUS_QUO_BLOB - peak) / 1048576.0, 3),
            "saving_pct_of_machine": round(
                100.0 * (STATUS_QUO_BLOB - peak) / DC_RAM, 2),
            "machine_ram_bytes": DC_RAM,
            "index_pct_of_machine": round(100.0 * resident / DC_RAM, 3),
        },
        "disc": {
            "pack_bytes": st["file_bytes"],
            "replaces_foresta_rel_bytes": A.REL_BLOB_SIZE,
            "disc_saving_vs_shipping_rel_bytes":
                A.REL_BLOB_SIZE - st["file_bytes"],
            "stream_seconds_at_500kb_s": round(
                st["file_bytes"] / 500000.0, 1),
        },
        "access_profile": prof,
        "access_profile_alternatives": alt,
        "readahead_simulation": ra,
        "swap_selftest": selftest,
        "roundtrip": rt,
        "timing_seconds": {
            "scan_and_extract": round(t_extract - t0, 2),
            "build": round(t_build - t_extract, 2),
            "verify_and_compare": round(t_verify - t_build, 2),
        },
    }
    (out_dir / "assets.pak.json").write_text(json.dumps(manifest, indent=2))
    return manifest


def load_blobs(image_path: str, gcm_mod) -> dict[int, bytes]:
    reader, info = gcm_mod.parse(image_path)
    try:
        dol = reader.read(info.dol_offset, info.dol_size)
        rel_e = next(f for f in info.files if f.path == "foresta.rel.szs")
        rel = gcm_mod.yaz0_decode(reader.read(rel_e.offset, rel_e.size))
    finally:
        reader.close()
    return {A.SRC_REL: rel, A.SRC_DOL: dol}


def verify_existing(pak_path: Path, image_path: str, repo: Path,
                    gcm_mod) -> dict:
    """Re-verify an already-built pack against the ISO and the source tree.

    Independent of the builder: it only reads the pack file, so it catches a
    truncated copy, a stale pack built from different sources, and any
    mismatch between the pack and what ``pc_load_asset`` expects.
    """
    image = Path(pak_path).read_bytes()
    refs, scan_stats = A.scan(repo)
    blobs = load_blobs(image_path, gcm_mod)
    rt = roundtrip(image, refs, blobs)
    idx = PackIndex(image)
    return {
        "pack_file": str(pak_path),
        "pack_bytes": len(image),
        "index_entries": len(idx.entries),
        "references": len(refs),
        "scan_unaccounted": scan_stats["src_unaccounted"],
        "swap_selftest": swap_selftest(),
        "roundtrip": rt,
    }
