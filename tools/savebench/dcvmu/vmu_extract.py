#!/usr/bin/env python3
"""Read a 128 KB VMU flash image and verify the OpenCrossing save on it.

WHY THIS EXISTS
---------------
The guest self-test in dc/src/dc_card.c asserts about itself. This reads the
bytes Flycast actually wrote to disk and re-derives everything independently on
the host: the VMU FAT chain, the VMS header CRC, the OCS1 container header, the
zlib chunks and the payload crc32. A guest that lies and a host parser that
lies the same way is not a plausible pair of bugs.

    python3 vmu_extract.py <run-dir>/home/.flycast/data/vmu_save_A1.bin
    python3 vmu_extract.py vmu_save_A1.bin --extract OXRT.TMP --out /tmp/x.bin

It is also the seed of the GCI interchange tool kb/save-plan.md §4.6 asks for:
the FAT walk and the container parse are the halves that tool needs.

Format sources: dc/vmufs.h (root/dir/FAT layout), dc/vmu_pkg.h (VMS header),
dc/include/dc_card.h (the OCS1 container). All little-endian.
"""
import argparse
import binascii
import struct
import sys
import zlib

BLOCK = 512
ROOT_BLOCK = 255

FAT_UNALLOCATED = 0xFFFA
FAT_LAST = 0xFFFC


def rd_root(img):
    """The root block names where the FAT and directory live. Read it rather
    than assuming 254/253: a card formatted by something other than the BIOS
    is allowed to differ, and silently reading the wrong block would produce
    confident nonsense."""
    b = img[ROOT_BLOCK * BLOCK:(ROOT_BLOCK + 1) * BLOCK]
    if b[:16] != b"\x55" * 16:
        raise ValueError("not a VMU image: root block magic is not 16 x 0x55")
    fat_loc, fat_size, dir_loc, dir_size, icon_shape, blk_cnt = struct.unpack_from(
        "<HHHHHH", b, 0x46)
    return dict(fat_loc=fat_loc, fat_size=fat_size, dir_loc=dir_loc,
                dir_size=dir_size, blk_cnt=blk_cnt)


def rd_fat(img, root):
    out = []
    for i in range(root["fat_size"]):
        blk = root["fat_loc"] - i
        out += list(struct.unpack_from("<256H", img, blk * BLOCK))
    return out


def rd_dir(img, root):
    ents = []
    for i in range(root["dir_size"]):
        blk = root["dir_loc"] - i
        base = blk * BLOCK
        for e in range(BLOCK // 32):
            off = base + e * 32
            ftype = img[off]
            if ftype not in (0x33, 0xCC):
                continue
            firstblk, = struct.unpack_from("<H", img, off + 2)
            name = img[off + 4:off + 16].split(b"\x00")[0].decode("ascii", "replace")
            size, hdroff = struct.unpack_from("<HH", img, off + 0x18)
            ents.append(dict(name=name, kind="game" if ftype == 0xCC else "data",
                             firstblk=firstblk, blocks=size, hdroff=hdroff))
    return ents


def read_file(img, fat, ent):
    """Files follow a FAT chain; blocks are NOT contiguous and typically run
    downward from the first block."""
    data = bytearray()
    blk = ent["firstblk"]
    seen = set()
    for _ in range(ent["blocks"]):
        if blk in (FAT_UNALLOCATED, FAT_LAST) or blk in seen:
            break
        seen.add(blk)
        data += img[blk * BLOCK:(blk + 1) * BLOCK]
        blk = fat[blk]
    return bytes(data)


EYECATCH = {0: 0, 1: 72 * 56 * 2, 2: 512 + 72 * 56, 3: 32 + 72 * 56 // 2}


def parse_vms(buf):
    desc_short = buf[0:16].decode("ascii", "replace").rstrip()
    desc_long = buf[16:48].decode("ascii", "replace").rstrip()
    app_id = buf[48:64].split(b"\x00")[0].decode("ascii", "replace").rstrip()
    icon_cnt, anim, ectype, crc, data_len = struct.unpack_from("<HHHHI", buf, 64)
    hdr_len = 128 + icon_cnt * 512 + EYECATCH.get(ectype, 0)

    # The VMS CRC is computed over the whole file with the CRC field zeroed
    # (vmu_pkg.c). Recomputing it is the first of the two integrity gates.
    whole = bytearray(buf[:hdr_len + data_len])
    whole[70] = 0
    whole[71] = 0
    n = 0
    for byte in whole:
        n ^= byte << 8
        for _ in range(8):
            n = ((n << 1) ^ 4129) & 0xFFFF if n & 0x8000 else (n << 1) & 0xFFFF
    return dict(desc_short=desc_short, desc_long=desc_long, app_id=app_id,
                icon_cnt=icon_cnt, eyecatch=ectype, crc=crc, crc_calc=n,
                data_len=data_len, payload=bytes(buf[hdr_len:hdr_len + data_len]))


def parse_ocs1(p):
    if len(p) < 32 or p[:4] != b"OCS1":
        raise ValueError("payload is not an OCS1 container (magic %r)" % p[:4])
    (flags, hdr, raw_len, stored_len, raw_crc, stored_crc,
     cl2, n, _res) = struct.unpack_from("<HHIIIIHHI", p, 4)
    tbl = hdr
    data_off = tbl + 4 * n
    lens = list(struct.unpack_from("<%dI" % n, p, tbl))
    body = p[data_off:data_off + stored_len]
    if binascii.crc32(body) & 0xFFFFFFFF != stored_crc:
        raise ValueError("stored_crc mismatch: container is damaged")
    cb = raw_len if cl2 == 0 else (1 << cl2)
    out = bytearray()
    off = 0
    for ln in lens:
        chunk = body[off:off + ln]
        out += zlib.decompress(chunk) if flags & 1 else chunk
        off += ln
    if len(out) != raw_len:
        raise ValueError("length mismatch: got %d, header says %d" % (len(out), raw_len))
    if binascii.crc32(out) & 0xFFFFFFFF != raw_crc:
        raise ValueError("raw_crc mismatch: the decompressed payload is wrong")
    return dict(flags=flags, raw_len=raw_len, stored_len=stored_len,
                chunk_bytes=cb, chunks=n, data=bytes(out))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="128 KB VMU flash image")
    ap.add_argument("--extract", metavar="NAME", help="write this file's payload out")
    ap.add_argument("--out", metavar="PATH", help="where --extract writes")
    a = ap.parse_args()

    img = open(a.image, "rb").read()
    if len(img) < 256 * BLOCK:
        sys.exit("image is %d bytes; a VMU is %d" % (len(img), 256 * BLOCK))

    root = rd_root(img)
    fat = rd_fat(img, root)
    ents = rd_dir(img, root)
    used = sum(e["blocks"] for e in ents)
    print("VMU %s: %d user blocks, %d entries, %d blocks used, %d free"
          % (a.image, root["blk_cnt"], len(ents), used, root["blk_cnt"] - used))

    rc = 0
    for e in ents:
        raw = read_file(img, fat, e)
        line = "  %-13s %-4s %3d blk (%6d B)" % (e["name"], e["kind"], e["blocks"],
                                                 e["blocks"] * BLOCK)
        try:
            vms = parse_vms(raw)
        except Exception as exc:
            print(line + "  [no VMS header: %s]" % exc)
            continue
        ok = "crc OK" if vms["crc"] == vms["crc_calc"] else \
             "CRC BAD (%04x != %04x)" % (vms["crc"], vms["crc_calc"])
        print(line + "  app_id=%-13s payload=%6d B  VMS %s"
              % (vms["app_id"], vms["data_len"], ok))
        if vms["crc"] != vms["crc_calc"]:
            rc = 1
        if not vms["payload"].startswith(b"OCS1"):
            continue
        try:
            c = parse_ocs1(vms["payload"])
        except Exception as exc:
            print("      OCS1: FAILED — %s" % exc)
            rc = 1
            continue
        print("      OCS1: %d B raw -> %d B stored in %d x %d B chunks "
              "(%.2f:1, %s), both crc32 OK"
              % (c["raw_len"], c["stored_len"], c["chunks"], c["chunk_bytes"],
                 c["raw_len"] / max(c["stored_len"], 1),
                 "deflate" if c["flags"] & 1 else "store"))
        if a.extract and e["name"] == a.extract:
            dst = a.out or (a.extract + ".bin")
            open(dst, "wb").write(c["data"])
            print("      wrote %d B to %s" % (len(c["data"]), dst))
    return rc


if __name__ == "__main__":
    sys.exit(main())
