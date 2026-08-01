"""GameCube disc image reader (ISO / GCM / CISO) for host-side tooling.

Faithful Python re-implementation of the parsing logic in
``pc/src/pc_disc.c`` (CISO block map, GCM header, FST walk, Yaz0), with the
runtime-only limits removed:

* ``pc_disc.c`` caps the FST at ``MAX_FST_FILES 1024`` and the directory
  stack at 32 -- fine for GAFE01 (10 files, 1 dir level), not general.
* ``pc_disc.c`` searches ``.``/``orig``/``rom`` for an image; the host tool
  always takes an explicit path.

Nothing here writes anything. Read-only by construction.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import BinaryIO

# ---------------------------------------------------------------- constants

GC_MAGIC = 0xC2339F3D          # at 0x1C in the boot header
CISO_HDR_SIZE = 0x8000
CISO_MAGIC = b"CISO"
CISO_MAP_OFF = 8

BOOT_BIN_SIZE = 0x440          # boot.bin  (disc header)
BI2_BIN_SIZE = 0x2000          # bi2.bin   (disc header information)
APPLOADER_OFF = 0x2440
APPLOADER_HDR_SIZE = 0x20

GC_DISC_SIZE = 1_459_978_240   # nominal single-layer GameCube image


def _be32(b: bytes, off: int = 0) -> int:
    return struct.unpack_from(">I", b, off)[0]


class DiscError(Exception):
    pass


# ------------------------------------------------------------------- reader


class DiscReader:
    """Random-access reader over a plain ISO/GCM or a CISO."""

    def __init__(self, path: str):
        self.path = path
        self.fp: BinaryIO = open(path, "rb")
        self.fp.seek(0, 2)
        self.file_size = self.fp.tell()
        self.fp.seek(0)

        self.is_ciso = False
        self.block_size = 0
        self.block_phys: list[int] = []

        hdr = self.fp.read(CISO_HDR_SIZE)
        if len(hdr) == CISO_HDR_SIZE and hdr[:4] == CISO_MAGIC:
            bs = struct.unpack_from("<I", hdr, 4)[0]
            if bs > 0:
                self.block_size = bs
                phys = 0
                nblk = CISO_HDR_SIZE - CISO_MAP_OFF
                self.block_phys = [0] * nblk
                for i in range(nblk):
                    if hdr[CISO_MAP_OFF + i]:
                        self.block_phys[i] = phys
                        phys += 1
                    else:
                        self.block_phys[i] = -1
                self.is_ciso = True
                self.ciso_used_blocks = phys

    # -- lifecycle ---------------------------------------------------------

    def close(self) -> None:
        if self.fp:
            self.fp.close()

    def __enter__(self) -> "DiscReader":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    # -- io ----------------------------------------------------------------

    @property
    def kind(self) -> str:
        return "CISO" if self.is_ciso else "ISO/GCM"

    def read(self, offset: int, size: int) -> bytes:
        """Read ``size`` bytes at logical disc ``offset``.

        Short reads raise; absent CISO blocks read back as zeroes, exactly
        as ``pc_disc.c`` does.
        """
        if size < 0:
            raise DiscError("negative read size")
        if size == 0:
            return b""

        if not self.is_ciso:
            self.fp.seek(offset)
            data = self.fp.read(size)
            if len(data) != size:
                raise DiscError(
                    f"short read at 0x{offset:X}: wanted {size}, got {len(data)}"
                )
            return data

        out = bytearray()
        while size > 0:
            bi, bo = divmod(offset, self.block_size)
            chunk = min(self.block_size - bo, size)
            if bi >= len(self.block_phys) or self.block_phys[bi] < 0:
                out += b"\x00" * chunk
            else:
                phys = CISO_HDR_SIZE + self.block_phys[bi] * self.block_size + bo
                self.fp.seek(phys)
                d = self.fp.read(chunk)
                if len(d) != chunk:
                    raise DiscError(f"short CISO read at 0x{offset:X}")
                out += d
            offset += chunk
            size -= chunk
        return bytes(out)

    def read_into_stream(self, offset: int, size: int, dst, chunk_size: int = 1 << 20):
        """Stream a range out to a writable binary file object."""
        remaining = size
        while remaining > 0:
            n = min(chunk_size, remaining)
            dst.write(self.read(offset, n))
            offset += n
            remaining -= n


# --------------------------------------------------------------------- Yaz0


def yaz0_is(data: bytes) -> bool:
    return len(data) >= 16 and data[:4] == b"Yaz0"


def yaz0_decompressed_size(data: bytes) -> int:
    if not yaz0_is(data):
        raise DiscError("not a Yaz0 stream")
    return _be32(data, 4)


def yaz0_decode(src: bytes) -> bytes:
    """Yaz0/SZS decoder. Byte-identical to ``pc_disc.c``'s ``yaz0_decode``."""
    if not yaz0_is(src):
        raise DiscError("not a Yaz0 stream")
    dec_size = _be32(src, 4)
    dst = bytearray(dec_size)
    src_size = len(src)
    sp = 16
    dp = 0
    while dp < dec_size and sp < src_size:
        flags = src[sp]
        sp += 1
        for bit in range(7, -1, -1):
            if dp >= dec_size or sp >= src_size:
                break
            if flags & (1 << bit):
                dst[dp] = src[sp]
                sp += 1
                dp += 1
            else:
                if sp + 1 >= src_size:
                    break
                b1 = src[sp]
                b2 = src[sp + 1]
                sp += 2
                dist = ((b1 & 0x0F) << 8) | b2
                if (b1 >> 4) == 0:
                    if sp >= src_size:
                        break
                    length = src[sp] + 0x12
                    sp += 1
                else:
                    length = (b1 >> 4) + 2
                ref = dp - dist - 1
                # overlapping copy must stay byte-by-byte
                end = min(dp + length, dec_size)
                while dp < end:
                    dst[dp] = dst[ref]
                    dp += 1
                    ref += 1
    return bytes(dst)


# ---------------------------------------------------------------------- FST


@dataclass
class FstEntry:
    path: str
    offset: int
    size: int
    index: int

    @property
    def end(self) -> int:
        return self.offset + self.size

    @property
    def ext(self) -> str:
        base = self.path.rsplit("/", 1)[-1]
        return base[base.index("."):].lower() if "." in base else "(none)"

    @property
    def directory(self) -> str:
        return self.path.rsplit("/", 1)[0] if "/" in self.path else "(root)"


@dataclass
class DiscInfo:
    path: str
    kind: str
    file_size: int
    game_id: str            # 6 chars, e.g. "GAFE01"
    disc_number: int
    version: int
    game_name: str
    dol_offset: int
    dol_size: int
    fst_offset: int
    fst_size: int
    fst_max_size: int
    fst_entry_count: int
    apploader_size: int     # total incl. 0x20 header + trailer
    user_pos: int
    user_len: int
    files: list[FstEntry] = field(default_factory=list)

    @property
    def is_gafe01_rev0(self) -> bool:
        return self.game_id == "GAFE01" and self.disc_number == 0 and self.version == 0


def dol_size(reader: DiscReader, dol_off: int) -> int:
    """Highest section end in the DOL header -- same math as pc_disc.c."""
    hdr = reader.read(dol_off, 0xE4)
    max_end = 0
    for i in range(7):                      # text sections
        off = _be32(hdr, i * 4)
        sz = _be32(hdr, 0x90 + i * 4)
        max_end = max(max_end, off + sz)
    for i in range(11):                     # data sections
        off = _be32(hdr, 0x1C + i * 4)
        sz = _be32(hdr, 0xAC + i * 4)
        max_end = max(max_end, off + sz)
    return max_end


def parse(path: str) -> tuple[DiscReader, DiscInfo]:
    """Open a disc image and parse header + FST. Caller closes the reader."""
    reader = DiscReader(path)

    boot = reader.read(0, BOOT_BIN_SIZE)
    if _be32(boot, 0x1C) != GC_MAGIC:
        reader.close()
        raise DiscError(
            f"{path}: not a GameCube disc image "
            f"(magic at 0x1C = 0x{_be32(boot, 0x1C):08X}, want 0x{GC_MAGIC:08X})"
        )

    game_id = boot[0:6].decode("ascii", "replace")
    disc_number = boot[6]
    version = boot[7]
    game_name = boot[0x20:0x60].split(b"\x00")[0].decode("ascii", "replace")

    dol_off = _be32(boot, 0x420)
    fst_off = _be32(boot, 0x424)
    fst_sz = _be32(boot, 0x428)
    fst_max = _be32(boot, 0x42C)
    user_pos = _be32(boot, 0x434)
    user_len = _be32(boot, 0x438)

    apl = reader.read(APPLOADER_OFF, APPLOADER_HDR_SIZE)
    apl_size = struct.unpack_from(">I", apl, 0x14)[0]
    apl_trailer = struct.unpack_from(">I", apl, 0x18)[0]
    apploader_total = APPLOADER_HDR_SIZE + apl_size + apl_trailer

    fst = reader.read(fst_off, fst_sz)
    num_ent = _be32(fst, 8)
    str_tbl = num_ent * 12
    if str_tbl > len(fst):
        # header's fst_size can under-report on hand-built images; re-read
        fst = reader.read(fst_off, str_tbl + 0x10000)

    files: list[FstEntry] = []
    # (next_entry, name) stack; root occupies slot 0 with an empty name
    stack: list[tuple[int, str]] = [(num_ent, "")]
    for i in range(1, num_ent):
        while len(stack) > 1 and i >= stack[-1][0]:
            stack.pop()
        e = fst[i * 12:i * 12 + 12]
        typ = e[0]
        noff = (e[1] << 16) | (e[2] << 8) | e[3]
        a = _be32(e, 4)
        b = _be32(e, 8)
        raw = fst[str_tbl + noff:]
        name = raw.split(b"\x00")[0].decode("ascii", "replace")
        if typ == 1:
            stack.append((b, name))
        else:
            prefix = "".join(n + "/" for _, n in stack[1:])
            files.append(FstEntry(prefix + name, a, b, i))

    info = DiscInfo(
        path=path,
        kind=reader.kind,
        file_size=reader.file_size,
        game_id=game_id,
        disc_number=disc_number,
        version=version,
        game_name=game_name,
        dol_offset=dol_off,
        dol_size=dol_size(reader, dol_off),
        fst_offset=fst_off,
        fst_size=fst_sz,
        fst_max_size=fst_max,
        fst_entry_count=num_ent,
        apploader_size=apploader_total,
        user_pos=user_pos,
        user_len=user_len,
        files=files,
    )
    return reader, info
