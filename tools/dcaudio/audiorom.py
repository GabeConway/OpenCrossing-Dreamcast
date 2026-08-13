"""Host-side reader for `audiorom.img` — the offline half of the AICA offload.

`audiorom.img` (8,300,384 B on the disc) HAS NO INDEX IN IT.  Every table that
says where anything lives is a static C array compiled into the game, in
`src/static/jaudio_NES/game/audioheaders.c`.  So this module reads two files:
the binary for payload and that C file for structure.  Parsing generated C is
ugly, but the alternative is transcribing 4,200 lines of tables by hand and
letting them rot — the C file is the authority, so read the authority.

    ┌ audioheaders.c ─────────────────────────────────────────────┐
    │ AudiodataHeaderStart   3 entries   the three extents         │
    │ AudioseqHeaderStart  249 entries   sequences   @ 0x000000    │
    │ AudiobankHeaderStart 159 entries   soundfonts  @ 0x0CF700    │
    │ AudiowaveHeaderStart   6 entries   sample banks @ 0x137380   │
    │ AudiomapHeaderStart  504 u16       sequence -> soundfont(s)  │
    └──────────────────────────────────────────────────────────────┘

An `ArcEntry.addr` is relative to its extent; `Nas_BankHeaderInit`
(`system.c:565-576`) adds the extent base at boot.  `AudiodataHeaderStart`
itself is never rebased, so its addrs are raw file offsets.

⭐ THE JOIN KEY, and the reason this tool can talk to the runtime at all:
`Nas_StartDma` computes `aram_offset = device_addr + GetNeosRomTop()`
(`system.c:1299`), and `GetNeosRomTop()` is a pure ARAM base address carrying
no file-offset component (`dummyrom.c:25-27`).  So a sample's `device_addr` IS
its raw byte offset into `audiorom.img`, and the runtime can recover it from a
live pointer by subtracting `GetNeosRomTop()`.  That offset is stable across
builds and computable here, which makes it the manifest key.

FOUR TRAPS, all of them things that will silently produce a wrong answer:

  * **Alias entries.** `size == 0` means `addr` is not an offset — it is the
    index of another entry (`__Link_BankNum`, `system.c:1023-1031`).  5
    sequences and 5 banks are aliases.  Treating an alias as an offset reads
    garbage from near the start of the extent, which parses.

  * **A ctl blob does NOT start with `voiceinfo`.**  `voiceinfo`
    (`audiostruct.h:396`) is a RAM-only struct built from the ArcEntry's param
    words by `__SetVlute` (`system.c:1488-1497`).  Offset 0 of the blob is a
    u32 offset table: `[0]` percussion, `[1]` sfx, `[2 + i]` instrument i.

  * **`medium` in an UNRELOCATED `smzwavetable` is a wave-bank selector, not a
    storage medium.**  0 picks `wave_bank_id0`, 1 picks `wave_bank_id1`.
    `__WaveTouch` overwrites the field with the real medium at
    `system.c:2123`/`:2128`.  Reading it as MEDIUM_RAM offline is wrong even
    though the value is 0.

  * **`loop` and `book` are ctl-blob-relative; `sample` is WAVE-BANK-relative.**
    Three different bases in one 16-byte struct.  `system.c:2113-2121`.

Everything here is big-endian and naturally aligned.
"""

from __future__ import annotations

import os
import re
import struct
from dataclasses import dataclass, field
from typing import Dict, Iterator, List, Optional, Sequence, Tuple

from .vadpcm import decode, frame_bytes, parse_book

# audiocommon.h:287-296
CODEC_ADPCM = 0
CODEC_S8 = 1
CODEC_S16_INMEMORY = 2
CODEC_SMALL_ADPCM = 3
CODEC_REVERB = 4
CODEC_S16 = 5

CODEC_NAMES = {
    CODEC_ADPCM: "ADPCM",
    CODEC_S8: "S8",
    CODEC_S16_INMEMORY: "S16_INMEMORY",
    CODEC_SMALL_ADPCM: "SMALL_ADPCM",
    CODEC_REVERB: "REVERB",
    CODEC_S16: "S16",
}

# audiocommon.h:210-216
MEDIUM_CART = 2

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_HEADERS = os.path.join(
    REPO, "src", "static", "jaudio_NES", "game", "audioheaders.c"
)
DEFAULT_IMG = os.path.expanduser("~/.cache/oc-dc-discroot/audiorom.img")


# ---------------------------------------------------------------------------
# audioheaders.c
# ---------------------------------------------------------------------------

@dataclass
class ArcEntry:
    """audiostruct.h:83-91, 0x10 bytes."""
    addr: int
    size: int
    medium: int
    cache_type: int
    param0: int
    param1: int
    param2: int

    @property
    def is_alias(self) -> bool:
        return self.size == 0

    # __SetVlute, system.c:1488-1497 — a soundfont's shape lives in the
    # ArcEntry's param words, not anywhere in the file.
    @property
    def wave_bank_id0(self) -> int:
        return (self.param0 >> 8) & 0xFF

    @property
    def wave_bank_id1(self) -> int:
        return self.param0 & 0xFF

    @property
    def num_instruments(self) -> int:
        return (self.param1 >> 8) & 0xFF

    @property
    def num_drums(self) -> int:
        return self.param1 & 0xFF

    @property
    def num_sfx(self) -> int:
        return self.param2


_MEDIUM_VALUES = {
    "MEDIUM_RAM": 0, "MEDIUM_DISK": 1, "MEDIUM_CART": 2,
    "MEDIUM_DISK_DRIVE": 3, "MEDIUM_RAM_UNLOADED": 5,
}
_CACHE_VALUES = {
    "CACHE_LOAD_PERMANENT": 0, "CACHE_LOAD_PERSISTENT": 1,
    "CACHE_LOAD_TEMPORARY": 2, "CACHE_LOAD_EITHER": 3,
    "CACHE_LOAD_EITHER_NOSYNC": 4,
}


def _num(tok: str) -> int:
    tok = tok.strip()
    if tok in _MEDIUM_VALUES:
        return _MEDIUM_VALUES[tok]
    if tok in _CACHE_VALUES:
        return _CACHE_VALUES[tok]
    if tok in ("nullptr", "NULL"):
        return 0
    return int(tok, 0)


def parse_audioheaders(path: str = DEFAULT_HEADERS) -> Dict[str, object]:
    """Parse the five tables out of the generated C.

    The file is machine-generated and rigidly regular (one initializer per
    line, `/* comment */` after each), so a line-oriented parse is safe here in
    a way it would not be for hand-written C.  Comments and `{}` nesting are
    stripped, then each header's brace group is split into 7-value records.
    """
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)

    out: Dict[str, object] = {}

    for name in ("AudioseqHeaderStart", "AudiobankHeaderStart",
                 "AudiowaveHeaderStart", "AudiodataHeaderStart"):
        m = re.search(
            rf"ArcHeader\s+{name}\s*(?:ATTRIBUTE_ALIGN\(\d+\)\s*)?=\s*\{{",
            text,
        )
        if not m:
            raise ValueError(f"{name} not found in {path}")
        body, _ = _brace_group(text, m.end() - 1)

        # Prologue is numEntries, medium, pData, copy, pad[7] (audiostruct.h:
        # 94-101), then the entries array. The pad is the FIRST brace group, so
        # skip past it to find the entries group.
        _, pad_end = _brace_group(body, body.index("{"))
        ent_start = body.index("{", pad_end)
        ent_body, _ = _brace_group(body, ent_start)

        head_nums = [
            _num(t) for t in body[:body.index("{")].split(",") if t.strip()
        ]
        n_entries = head_nums[0]

        entries: List[ArcEntry] = []
        pos = 0
        while True:
            try:
                s = ent_body.index("{", pos)
            except ValueError:
                break
            rec, pos = _brace_group(ent_body, s)
            vals = [_num(t) for t in rec.split(",") if t.strip()]
            if len(vals) != 7:
                raise ValueError(
                    f"{name}: entry {len(entries)} has {len(vals)} fields, "
                    f"expected 7"
                )
            entries.append(ArcEntry(*vals))

        if len(entries) != n_entries:
            raise ValueError(
                f"{name}: header declares {n_entries} entries, parsed "
                f"{len(entries)}"
            )
        out[name] = entries

    m = re.search(
        r"u16\s+AudiomapHeaderStart\s*\[\s*\]\s*(?:ATTRIBUTE_ALIGN\(\d+\)\s*)?"
        r"=\s*\{",
        text,
    )
    if not m:
        raise ValueError(f"AudiomapHeaderStart not found in {path}")
    body, _ = _brace_group(text, m.end() - 1)
    out["AudiomapHeaderStart"] = [
        _num(t) for t in body.split(",") if t.strip()
    ]

    return out


def _brace_group(text: str, open_idx: int) -> Tuple[str, int]:
    """Return (contents-between-braces, index-just-past-close)."""
    assert text[open_idx] == "{"
    depth = 0
    i = open_idx
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[open_idx + 1:i], i + 1
        i += 1
    raise ValueError("unbalanced braces")


# ---------------------------------------------------------------------------
# the bank
# ---------------------------------------------------------------------------

@dataclass
class AdpcmLoop:
    """audiostruct.h:104-110. 0x10 bytes, or 0x30 when `count != 0`.

    `count` is read unsigned: 0xFFFFFFFF means loop forever and DOES carry the
    16-entry `predictor_state` (system.c:128-132).
    """
    loop_start: int
    loop_end: int
    count: int
    sample_end: int
    predictor_state: Optional[List[int]] = None

    @property
    def loops(self) -> bool:
        return self.count != 0


@dataclass
class Sample:
    """One unique VADPCM sample, keyed by its position in the file."""
    wave_bank: int
    sample_ofs: int          # relative to the wave bank's base
    file_off: int            # raw byte offset into audiorom.img == device_addr
    size: int                # encoded byte count, from the 24-bit size field
    codec: int
    loop: AdpcmLoop
    book_order: int
    book_npredictors: int
    book: List[List[int]]
    tunings: List[float] = field(default_factory=list)
    banks: List[int] = field(default_factory=list)

    @property
    def key(self) -> Tuple[int, int, int]:
        return (self.wave_bank, self.sample_ofs, self.size)

    @property
    def n_samples(self) -> int:
        """Decoded length in samples.

        `loop.sample_end` is the engine's own count and is authoritative; fall
        back to the frame arithmetic when it is absent (one-shots record 0).
        """
        if self.loop.sample_end:
            return self.loop.sample_end
        fsz = frame_bytes(self.codec == CODEC_SMALL_ADPCM)
        return (self.size // fsz) * 16


class AudioRom:
    def __init__(self, img_path: str = DEFAULT_IMG,
                 headers_path: str = DEFAULT_HEADERS):
        with open(img_path, "rb") as fh:
            self.img = fh.read()
        self.hdr = parse_audioheaders(headers_path)

        data: List[ArcEntry] = self.hdr["AudiodataHeaderStart"]  # type: ignore
        if len(data) != 3:
            raise ValueError(f"expected 3 extents, got {len(data)}")
        self.seq_base, self.bank_base, self.wave_base = (e.addr for e in data)
        self.extents = [(e.addr, e.size) for e in data]

        total = sum(e.size for e in data)
        if total != len(self.img):
            raise ValueError(
                f"extents sum to {total:,} but the image is {len(self.img):,}"
            )

        self.seqs: List[ArcEntry] = self.hdr["AudioseqHeaderStart"]    # type: ignore
        self.banks: List[ArcEntry] = self.hdr["AudiobankHeaderStart"]  # type: ignore
        self.waves: List[ArcEntry] = self.hdr["AudiowaveHeaderStart"]  # type: ignore
        self.map: List[int] = self.hdr["AudiomapHeaderStart"]          # type: ignore

    # -- alias resolution (__Link_BankNum, system.c:1023-1031) --------------
    def resolve(self, table: Sequence[ArcEntry], idx: int) -> int:
        seen = set()
        while table[idx].is_alias:
            if idx in seen:
                raise ValueError(f"alias cycle at index {idx}")
            seen.add(idx)
            idx = table[idx].addr
        return idx

    def bank_blob(self, bank_id: int) -> Tuple[int, ArcEntry]:
        """(file offset of the ctl blob, the entry that describes it).

        The alias target supplies the bytes; the ORIGINAL entry supplies the
        param words, because `__SetVlute` reads them off the entry the caller
        asked for.
        """
        real = self.resolve(self.banks, bank_id)
        return self.bank_base + self.banks[real].addr, self.banks[bank_id]

    # -- readers -----------------------------------------------------------
    def _u32(self, off: int) -> int:
        return struct.unpack_from(">I", self.img, off)[0]

    def _s32(self, off: int) -> int:
        return struct.unpack_from(">i", self.img, off)[0]

    def _f32(self, off: int) -> float:
        return struct.unpack_from(">f", self.img, off)[0]

    def read_loop(self, off: int) -> AdpcmLoop:
        ls, le, cnt, se = struct.unpack_from(">IIII", self.img, off)
        state = None
        if cnt != 0:
            state = list(struct.unpack_from(">16h", self.img, off + 16))
        return AdpcmLoop(ls, le, cnt, se, state)

    def read_book(self, off: int) -> Tuple[int, int, List[List[int]]]:
        order = self._s32(off)
        npred = self._s32(off + 4)
        if not (1 <= order <= 8) or not (1 <= npred <= 8):
            raise ValueError(
                f"implausible book at 0x{off:X}: order={order} npred={npred}"
            )
        n = order * npred * 8
        return order, npred, parse_book(
            self.img[off + 8: off + 8 + n * 2], order, npred
        )

    def read_wavetable(self, ctl_base: int, wt_off: int,
                       entry: ArcEntry) -> Sample:
        """Decode one `smzwavetable` (audiostruct.h:119-143) at ctl+wt_off."""
        off = ctl_base + wt_off
        word = self._u32(off)
        # bit31:1 codec:3 medium:2 bit26:1 is_relocated:1 size:24
        codec = (word >> 28) & 0x7
        selector = (word >> 26) & 0x3     # NOT a medium -- wave-bank selector
        size = word & 0x00FFFFFF
        sample_ofs = self._u32(off + 4)
        loop_ofs = self._u32(off + 8)
        book_ofs = self._u32(off + 12)

        wb = entry.wave_bank_id0 if selector == 0 else entry.wave_bank_id1
        if wb == 0xFF:
            raise ValueError(
                f"wavetable at ctl+0x{wt_off:X} selects wave bank "
                f"{selector} but the soundfont declares none"
            )
        wreal = self.resolve(self.waves, wb)
        file_off = self.wave_base + self.waves[wreal].addr + sample_ofs

        order, npred, book = self.read_book(ctl_base + book_ofs)
        return Sample(
            wave_bank=wb,
            sample_ofs=sample_ofs,
            file_off=file_off,
            size=size,
            codec=codec,
            loop=self.read_loop(ctl_base + loop_ofs),
            book_order=order,
            book_npredictors=npred,
            book=book,
        )

    # -- enumeration -------------------------------------------------------
    def bank_wavetable_offsets(self, bank_id: int) -> List[int]:
        """Every ctl-relative `smzwavetable` offset a soundfont references."""
        ctl, entry = self.bank_blob(bank_id)
        offs: List[int] = []

        # ctrl[0] percussion, ctrl[1] sfx, ctrl[2+i] instrument i.
        # system.c:1088-1160.
        n_inst = min(entry.num_instruments, 126)

        perc_tbl = self._u32(ctl + 0)
        if perc_tbl and entry.num_drums:
            for d in range(entry.num_drums):
                p = self._u32(ctl + perc_tbl + d * 4)
                if not p:
                    continue
                # perctable, audiostruct.h:382-388: wtstr at +0x04
                offs.append(self._u32(ctl + p + 4))

        sfx_tbl = self._u32(ctl + 4)
        if sfx_tbl and entry.num_sfx:
            for s in range(entry.num_sfx):
                # percvoicetable is just a wtstr, 8 B
                offs.append(self._u32(ctl + sfx_tbl + s * 8))

        for i in range(n_inst):
            v = self._u32(ctl + 8 + i * 4)
            if not v:
                continue
            # voicetable, audiostruct.h:370-379: three wtstr at +0x08/+0x10/+0x18
            for w in (0x08, 0x10, 0x18):
                wt = self._u32(ctl + v + w)
                if wt:
                    offs.append(wt)

        return [o for o in offs if o]

    def bank_samples(self, bank_id: int) -> List[Sample]:
        ctl, entry = self.bank_blob(bank_id)
        out: List[Sample] = []
        for wt in dict.fromkeys(self.bank_wavetable_offsets(bank_id)):
            try:
                out.append(self.read_wavetable(ctl, wt, entry))
            except (ValueError, struct.error):
                continue
        return out

    def unique_samples(self) -> Dict[Tuple[int, int, int], Sample]:
        """Every distinct sample in the game, deduped across soundfonts."""
        uniq: Dict[Tuple[int, int, int], Sample] = {}
        for b in range(len(self.banks)):
            for s in self.bank_samples(b):
                cur = uniq.setdefault(s.key, s)
                if b not in cur.banks:
                    cur.banks.append(b)
        return uniq

    # -- sequence -> soundfont (Nas_PreLoadBank, system.c:578-593) ----------
    def _map_byte(self, i: int) -> int:
        """AudiomapHeaderStart is u16[] read as a BIG-ENDIAN byte array.

        `Nas_MapHeaderReadByte` (system.c:277-288) indexes it per byte, so the
        u16 at [i//2] contributes its high byte at even i and its low byte at
        odd i.
        """
        w = self.map[i >> 1]
        return (w >> 8) & 0xFF if (i & 1) == 0 else w & 0xFF

    def seq_banks(self, seq_id: int) -> List[int]:
        idx = self.map[seq_id]
        count = self._map_byte(idx)
        return [self._map_byte(idx + 1 + k) for k in range(count)]

    def decode_sample(self, s: Sample) -> List[int]:
        """Decode one sample to PCM16, cold-started like a fresh key-on."""
        return decode(
            self.img[s.file_off: s.file_off + s.size],
            s.book,
            s.n_samples,
            is_small=(s.codec == CODEC_SMALL_ADPCM),
        )

    def iter_samples(self) -> Iterator[Sample]:
        yield from self.unique_samples().values()
