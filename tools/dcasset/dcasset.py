#!/usr/bin/env python3
"""dcasset -- GameCube disc image -> Dreamcast disc layout.

The Anbernic/PC port parses the GameCube ISO at *runtime* (``pc/src/pc_disc.c``
+ ``pc_dvd.c``). The Dreamcast cannot: a stock console has 16 MB of RAM and
reads its own ISO9660 filesystem off CD-R at ~500 KB/s. So all disc parsing
moves offline into this tool, which emits a plain directory tree that
``mkdcdisc`` turns into a CDI.

Modes
-----
  report    measure the image: real content vs padding, per-file breakdown,
            compressibility. Answers "how much of the 1.46 GB is real?"
  extract   write a Dreamcast-ready directory tree + manifest.json
  verify    re-hash an extracted tree against its manifest

The user's ISO is *read only*, never copied into the repo, and the default
output path is deliberately outside the repository.

Usage
-----
  python3 dcasset.py report  "/path/Animal Crossing.iso"
  python3 dcasset.py report  "/path/Animal Crossing.iso" --json out.json
  python3 dcasset.py extract "/path/Animal Crossing.iso" --out /tmp/dcroot
  python3 dcasset.py verify  /tmp/dcroot
"""

from __future__ import annotations

import argparse
import hashlib
import json
import lzma
import os
import sys
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import gcm  # noqa: E402

MANIFEST_VERSION = 1
DEFAULT_OUT = os.environ.get("DCASSET_OUT", "/tmp/opencrossing-dc/discroot")

# The one image this port is built from. Anything else is refused unless
# --force is given, because every asset offset downstream assumes it.
EXPECTED_GAME_ID = "GAFE01"
EXPECTED_DISC = 0
EXPECTED_VERSION = 0

# Files that end up on the Dreamcast disc. foresta.rel.szs is special-cased:
# it is Yaz0-decompressed at extract time (see notes in README).
REL_SZS = "foresta.rel.szs"
REL_OUT = "foresta.rel"

# Repo root = tools/dcasset/../..  -- used to refuse writing assets into git.
REPO_ROOT = Path(__file__).resolve().parent.parent.parent


# --------------------------------------------------------------- formatting


def mib(n: int) -> str:
    return f"{n / 1048576:.2f} MB"


def commas(n: int) -> str:
    return f"{n:,}"


def pct(a: int, b: int) -> str:
    return "n/a" if b == 0 else f"{100.0 * a / b:.2f}%"


def magic_of(data: bytes) -> str:
    m = data[:4]
    printable = all(32 <= c < 127 for c in m)
    return m.decode("ascii") if printable else " ".join(f"{c:02X}" for c in m)


# ------------------------------------------------------------------ guards


def check_out_path(out: Path) -> Path:
    out = out.resolve()
    try:
        out.relative_to(REPO_ROOT)
    except ValueError:
        return out
    sys.exit(
        f"refusing to extract into the repository ({out}).\n"
        f"Extracted GameCube assets must never be committed. "
        f"Pick a path outside {REPO_ROOT} (default: {DEFAULT_OUT})."
    )


def validate(info: gcm.DiscInfo, force: bool) -> None:
    ok = (
        info.game_id == EXPECTED_GAME_ID
        and info.disc_number == EXPECTED_DISC
        and info.version == EXPECTED_VERSION
    )
    if ok:
        return
    msg = (
        f"image is {info.game_id} disc {info.disc_number} rev {info.version}; "
        f"this port targets {EXPECTED_GAME_ID} disc {EXPECTED_DISC} "
        f"rev {EXPECTED_VERSION} (USA). Asset offsets will not match."
    )
    if force:
        print(f"WARNING: {msg}", file=sys.stderr)
    else:
        sys.exit(f"error: {msg}\n(pass --force to proceed anyway)")


# ------------------------------------------------------------------ measure


def measure(reader: gcm.DiscReader, info: gcm.DiscInfo, do_lzma: bool) -> dict:
    """Build the full measurement dict that both report and --json emit."""
    sysarea = [
        ("boot.bin (disc header)", 0, gcm.BOOT_BIN_SIZE),
        ("bi2.bin (disc info)", gcm.BOOT_BIN_SIZE, gcm.BI2_BIN_SIZE),
        ("apploader.img", gcm.APPLOADER_OFF, info.apploader_size),
        ("main.dol", info.dol_offset, info.dol_size),
        ("fst.bin", info.fst_offset, info.fst_size),
    ]
    sys_total = sum(s for _, _, s in sysarea)

    files = []
    for e in sorted(info.files, key=lambda f: f.offset):
        head = reader.read(e.offset, min(64, e.size)) if e.size else b""
        rec = {
            "path": e.path,
            "offset": e.offset,
            "size": e.size,
            "ext": e.ext,
            "dir": e.directory,
            "magic": magic_of(head),
            "yaz0": gcm.yaz0_is(head),
        }
        if rec["yaz0"]:
            rec["yaz0_decompressed_size"] = gcm.yaz0_decompressed_size(head)
        files.append(rec)

    fst_total = sum(f["size"] for f in files)

    # Accounting is done as an interval union over every claimed region, so
    # nothing is double counted. It has to be: the apploader's documented
    # extent (0x20 header + size + trailer) runs 32 bytes past where main.dol
    # actually starts on this master, so naive summing over-reports.
    regions = [(o, o + s) for _, o, s in sysarea if s > 0]
    regions += [(f["offset"], f["offset"] + f["size"]) for f in files
                if f["size"] > 0]
    merged: list[list[int]] = []
    for a, b in sorted(regions):
        if merged and a <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], b)
        else:
            merged.append([a, b])
    real_content = sum(b - a for a, b in merged)
    last_used = merged[-1][1]
    overlap = (sys_total + fst_total) - real_content
    # unclaimed bytes below the last used byte (alignment slack)
    gaps = last_used - real_content

    # ---- compressibility -------------------------------------------------
    # Concatenate everything a DC build actually needs and squeeze it. This is
    # the number that answers "does the payload fit comfortably on a CD-R" and
    # "was the ~16 MB recollection right".
    t0 = time.time()
    payload = bytearray()
    per_file_deflate = {}
    for name, off, size in sysarea:
        if name.startswith(("boot", "bi2", "apploader", "fst")):
            continue  # DC does not need GameCube system area
        blob = reader.read(off, size)
        payload += blob
        per_file_deflate[name] = len(zlib.compress(blob, 9))
    for f in files:
        blob = reader.read(f["offset"], f["size"])
        payload += blob
        per_file_deflate[f["path"]] = len(zlib.compress(blob, 9))
    payload = bytes(payload)

    comp = {
        "raw": len(payload),
        "deflate_9": len(zlib.compress(payload, 9)),
        "seconds": None,
    }
    if do_lzma:
        comp["lzma_6"] = len(lzma.compress(payload, preset=6))
    comp["seconds"] = round(time.time() - t0, 1)

    return {
        "image": {
            "path": info.path,
            "kind": info.kind,
            "file_size": info.file_size,
            "game_id": info.game_id,
            "disc_number": info.disc_number,
            "version": info.version,
            "game_name": info.game_name,
            "nominal_gc_size": gcm.GC_DISC_SIZE,
        },
        "system_area": [
            {"name": n, "offset": o, "size": s} for n, o, s in sysarea
        ],
        "system_area_total": sys_total,
        "fst": {
            "offset": info.fst_offset,
            "size": info.fst_size,
            "entry_count": info.fst_entry_count,
            "file_count": len(files),
            "user_pos": info.user_pos,
            "user_len": info.user_len,
        },
        "files": files,
        "totals": {
            "fst_content": fst_total,
            "system_area": sys_total,
            "region_overlap": overlap,
            "real_content": real_content,
            "last_used_byte": last_used,
            "trailing_padding": info.file_size - last_used,
            "alignment_gaps": gaps,
            "used_regions": len(merged),
        },
        "compression": comp,
        "per_file_deflate_9": per_file_deflate,
    }


def by_key(files: list[dict], key: str) -> list[tuple[str, int, int]]:
    agg: dict[str, list[int]] = {}
    for f in files:
        a = agg.setdefault(f[key], [0, 0])
        a[0] += f["size"]
        a[1] += 1
    return sorted(((k, v[0], v[1]) for k, v in agg.items()),
                  key=lambda x: -x[1])


# ------------------------------------------------------------------- report


def cmd_report(args) -> int:
    reader, info = gcm.parse(args.image)
    try:
        validate(info, args.force)
        m = measure(reader, info, do_lzma=not args.no_lzma)
    finally:
        reader.close()

    img = m["image"]
    t = m["totals"]
    c = m["compression"]
    files = m["files"]

    W = 78
    print("=" * W)
    print("dcasset report")
    print("=" * W)
    print(f"image          : {img['path']}")
    print(f"container      : {img['kind']}")
    print(f"game id        : {img['game_id']}  disc {img['disc_number']}  "
          f"rev {img['version']}  ({img['game_name']})")
    print(f"file size      : {commas(img['file_size'])} bytes  "
          f"({mib(img['file_size'])}, {img['file_size'] / 1e9:.3f} GB)")
    print()

    print("-- PADDING VS CONTENT " + "-" * (W - 22))
    print(f"  total image                 {commas(img['file_size']):>14}  "
          f"{mib(img['file_size']):>11}")
    print(f"  GC system area (hdr/apl/dol/fst)"
          f"{commas(t['system_area']):>11}  {mib(t['system_area']):>11}")
    print(f"  FST file content            {commas(t['fst_content']):>14}  "
          f"{mib(t['fst_content']):>11}")
    print(f"  (region overlap, not counted twice)"
          f"{commas(t['region_overlap']):>8}")
    print(f"  = real content              {commas(t['real_content']):>14}  "
          f"{mib(t['real_content']):>11}  "
          f"{pct(t['real_content'], img['file_size'])} of image")
    print(f"  alignment gaps in used area {commas(t['alignment_gaps']):>14}  "
          f"{mib(t['alignment_gaps']):>11}")
    print(f"  last byte used at           {commas(t['last_used_byte']):>14}  "
          f"{mib(t['last_used_byte']):>11}  (0x{t['last_used_byte']:X})")
    print(f"  trailing padding            {commas(t['trailing_padding']):>14}  "
          f"{mib(t['trailing_padding']):>11}  "
          f"{pct(t['trailing_padding'], img['file_size'])} of image")
    print()

    print("-- GC SYSTEM AREA " + "-" * (W - 18))
    for s in m["system_area"]:
        print(f"  {s['name']:<34} 0x{s['offset']:08X} "
              f"{commas(s['size']):>12}  {mib(s['size']):>10}")
    print()

    hdr = "-- FST FILES (%d) " % len(files)
    print(hdr + "-" * (W - len(hdr)))
    print(f"  {'file':<20}{'offset':>10}  {'size':>12}  {'':>9}  magic")
    for f in files:
        note = f"  Yaz0 -> {commas(f['yaz0_decompressed_size'])}" \
            if f.get("yaz0") else ""
        print(f"  {f['path']:<20}0x{f['offset']:08X}  {commas(f['size']):>12}"
              f"  {mib(f['size']):>9}  {f['magic']}{note}")
    print()

    print("-- BY EXTENSION " + "-" * (W - 16))
    for ext, size, n in by_key(files, "ext"):
        print(f"  {ext:<20}{commas(size):>14}  {mib(size):>10}  "
              f"{pct(size, t['fst_content']):>7}  ({n} file(s))")
    print()

    print("-- BY DIRECTORY " + "-" * (W - 16))
    for d, size, n in by_key(files, "dir"):
        print(f"  {d:<20}{commas(size):>14}  {mib(size):>10}  "
              f"{pct(size, t['fst_content']):>7}  ({n} file(s))")
    print()

    print("-- LARGEST FILES " + "-" * (W - 17))
    for f in sorted(files, key=lambda x: -x["size"])[:5]:
        print(f"  {f['path']:<20}{commas(f['size']):>14}  {mib(f['size']):>10}  "
              f"{pct(f['size'], t['fst_content']):>7}")
    print()

    audio = sum(f["size"] for f in files if f["path"].startswith("audiorom"))
    print("-- AUDIO SHARE " + "-" * (W - 15))
    print(f"  audiorom.img                {commas(audio):>14}  {mib(audio):>10}"
          f"  {pct(audio, t['fst_content'])} of FST content, "
          f"{pct(audio, t['real_content'])} of real content")
    print()

    print("-- COMPRESSED (DC payload = main.dol + all FST files) " + "-" * 24)
    print(f"  raw                         {commas(c['raw']):>14}  "
          f"{mib(c['raw']):>10}")
    print(f"  deflate -9 (zlib)           {commas(c['deflate_9']):>14}  "
          f"{mib(c['deflate_9']):>10}  "
          f"{pct(c['deflate_9'], c['raw'])} of raw")
    if "lzma_6" in c:
        print(f"  lzma preset 6               {commas(c['lzma_6']):>14}  "
              f"{mib(c['lzma_6']):>10}  {pct(c['lzma_6'], c['raw'])} of raw")
    print(f"  (measured in {c['seconds']}s)")
    print()
    print("  per-file deflate -9:")
    for k, v in sorted(m["per_file_deflate_9"].items(), key=lambda x: -x[1]):
        raw_sz = next((f["size"] for f in files if f["path"] == k), None)
        if raw_sz is None:
            raw_sz = next(s["size"] for s in m["system_area"] if s["name"] == k)
        print(f"    {k:<24}{commas(raw_sz):>12} -> {commas(v):>12}  "
              f"{pct(v, raw_sz):>7}")
    print()

    rel = next((f for f in files if f["path"] == REL_SZS), None)
    cdr = 700 * 1000 * 1000
    dc_payload = c["raw"]
    if rel and rel.get("yaz0"):
        dc_payload = dc_payload - rel["size"] + rel["yaz0_decompressed_size"]
    print("-- DREAMCAST DISC SIZING " + "-" * (W - 25))
    print(f"  payload as-is (rel kept Yaz0)      {commas(c['raw']):>13}  "
          f"{mib(c['raw']):>10}")
    print(f"  payload with foresta.rel expanded  {commas(dc_payload):>13}  "
          f"{mib(dc_payload):>10}   <- recommended layout")
    print(f"  CD-R capacity (700 MB)             {commas(cdr):>13}  "
          f"{mib(cdr):>10}")
    print(f"  headroom                           "
          f"{commas(cdr - dc_payload):>13}  {mib(cdr - dc_payload):>10}  "
          f"({100.0 * dc_payload / cdr:.1f}% used)")
    print("=" * W)

    if args.json:
        m["dc_payload_rel_expanded"] = dc_payload
        Path(args.json).write_text(json.dumps(m, indent=2))
        print(f"json written: {args.json}")
    return 0


# ------------------------------------------------------------------ extract


def sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def cmd_extract(args) -> int:
    out = check_out_path(Path(args.out))
    reader, info = gcm.parse(args.image)
    try:
        validate(info, args.force)

        files_dir = out / "files"
        sys_dir = out / "sys"
        files_dir.mkdir(parents=True, exist_ok=True)
        sys_dir.mkdir(parents=True, exist_ok=True)

        entries = []

        def emit(rel_path: str, writer, source: str, note: str = "") -> None:
            dst = out / rel_path
            dst.parent.mkdir(parents=True, exist_ok=True)
            with open(dst, "wb") as fh:
                writer(fh)
            size = dst.stat().st_size
            entries.append({
                "path": rel_path,
                "size": size,
                "sha256": sha256_file(dst),
                "source": source,
                "note": note,
            })
            print(f"  {rel_path:<28} {commas(size):>12} bytes  {note}")

        print(f"extracting {info.game_id} -> {out}")

        # main.dol: the asset source for pc_assets.c's SRC_DOL entries.
        emit("sys/main.dol",
             lambda fh: reader.read_into_stream(info.dol_offset, info.dol_size, fh),
             f"dol@0x{info.dol_offset:X}",
             "GC executable; used as an asset blob, not run")

        if args.keep_sys:
            emit("sys/boot.bin",
                 lambda fh: reader.read_into_stream(0, gcm.BOOT_BIN_SIZE, fh),
                 "disc@0x0", "disc header (reference)")
            emit("sys/bi2.bin",
                 lambda fh: reader.read_into_stream(
                     gcm.BOOT_BIN_SIZE, gcm.BI2_BIN_SIZE, fh),
                 f"disc@0x{gcm.BOOT_BIN_SIZE:X}", "disc info (reference)")
            emit("sys/apploader.img",
                 lambda fh: reader.read_into_stream(
                     gcm.APPLOADER_OFF, info.apploader_size, fh),
                 f"disc@0x{gcm.APPLOADER_OFF:X}", "GC apploader (unused on DC)")
            emit("sys/fst.bin",
                 lambda fh: reader.read_into_stream(
                     info.fst_offset, info.fst_size, fh),
                 f"disc@0x{info.fst_offset:X}", "GC FST (reference)")

        for e in sorted(info.files, key=lambda f: f.offset):
            head = reader.read(e.offset, min(16, e.size)) if e.size else b""
            is_rel_szs = e.path == REL_SZS

            if is_rel_szs and not args.keep_szs:
                raw = reader.read(e.offset, e.size)
                dec = gcm.yaz0_decode(raw)
                emit(f"files/{REL_OUT}", lambda fh, d=dec: fh.write(d),
                     f"disc@0x{e.offset:X}",
                     f"Yaz0-decompressed ({commas(e.size)} -> {commas(len(dec))})")
                continue

            note = "Yaz0 (kept compressed)" if gcm.yaz0_is(head) else ""
            emit(f"files/{e.path}",
                 lambda fh, o=e.offset, s=e.size: reader.read_into_stream(o, s, fh),
                 f"disc@0x{e.offset:X}", note)

        total = sum(x["size"] for x in entries)
        manifest = {
            "manifest_version": MANIFEST_VERSION,
            "tool": "tools/dcasset/dcasset.py",
            "generated_unix": int(time.time()),
            "source_image": {
                "path": str(Path(args.image).resolve()),
                "kind": info.kind,
                "file_size": info.file_size,
                "game_id": info.game_id,
                "disc_number": info.disc_number,
                "version": info.version,
                "game_name": info.game_name,
            },
            "options": {
                "keep_szs": bool(args.keep_szs),
                "keep_sys": bool(args.keep_sys),
            },
            "total_size": total,
            "file_count": len(entries),
            "files": entries,
        }
        (out / "manifest.json").write_text(json.dumps(manifest, indent=2))
        print(f"\n  manifest.json                {len(entries)} entries")
        print(f"  total payload                {commas(total)} bytes "
              f"({mib(total)})")
        print(f"\nready for: mkdcdisc -d {out} -o OpenCrossing.cdi ...")
    finally:
        reader.close()
    return 0


# ------------------------------------------------------------------ relmap

# pc_assets.c pulls every static asset out of the decompressed REL (and a
# little out of main.dol) by absolute byte offset. On PC both blobs are held
# resident (15.6 MB + 0.9 MB) -- impossible on a 16 MB Dreamcast. relmap
# measures which bytes are actually referenced, so a later pass can emit only
# those spans and drop the resident blobs entirely.

# Generated call sites carry inline /* SRC_DOL */-style comments between the
# arguments, so every separator has to tolerate them. Missing that costs real
# call sites (it silently dropped one in JFWSystem.cpp).
_C = r'(?:\s|/\*.*?\*/)*'
_NUM = r'(0x[0-9A-Fa-f]+|\d+)'
_ARGS = (
    _C + _NUM + _C + r',' + _C + _NUM + _C + r',' + _C + r'(\d+)'
    + _C + r',' + _C + r'(\d+)' + _C
)
_CALL_RE = (
    r'pc_load_asset' + _C + r'\(' + _C + r'(?:"(?:[^"\\]|\\.)*"|NULL)'
    + _C + r',[^,]+,' + _ARGS + r'\)'
)
_TABLE_RE = (
    r'\{' + _C + r'(?:"(?:[^"\\]|\\.)*"|NULL)' + _C + r',[^,]+,'
    + _ARGS + r'\}'
)
SRC_REL, SRC_DOL = 0, 1


def cmd_relmap(args) -> int:
    import re

    repo = Path(args.repo).resolve()
    assets_c = repo / "pc" / "src" / "pc_assets.c"
    if not assets_c.exists():
        sys.exit(f"not found: {assets_c}")

    ranges: dict[int, list[tuple[int, int]]] = {SRC_REL: [], SRC_DOL: []}
    n = 0

    txt = assets_c.read_text(errors="replace")
    marker = "static const PCAsset s_assets[] = {"
    if marker in txt:
        i = txt.index(marker)
        j = txt.index("\n};", i)
        for m in re.finditer(_TABLE_RE, txt[i:j]):
            size, off, src = int(m.group(1), 0), int(m.group(2), 0), int(m.group(3))
            if src in ranges:
                ranges[src].append((off, off + size))
                n += 1

    call_re = re.compile(_CALL_RE, re.S)
    # every textual occurrence, so unmatched ones can be reported rather than
    # silently dropped
    occ_re = re.compile(r'pc_load_asset\s*\(')
    proto_re = re.compile(r'pc_load_asset\s*\(\s*const\s+char\s*\*')
    occurrences = 0
    prototypes = 0
    src_matched = 0
    for root, _dirs, fnames in os.walk(repo / "src"):
        for fn in fnames:
            p = Path(root) / fn
            try:
                t = p.read_text(errors="replace")
            except OSError:
                continue
            if "pc_load_asset" not in t:
                continue
            occurrences += len(occ_re.findall(t))
            prototypes += len(proto_re.findall(t))
            for m in call_re.finditer(t):
                size, off, src = int(m.group(1), 0), int(m.group(2), 0), int(m.group(3))
                src_matched += 1
                if src in ranges:
                    ranges[src].append((off, off + size))
                    n += 1

    unaccounted = occurrences - prototypes - src_matched
    print(f"parsed {n} pc_load_asset references "
          f"({len(ranges[SRC_REL])} REL + {len(ranges[SRC_DOL])} DOL)")
    print(f"  src/ tree: {occurrences} textual occurrences = "
          f"{prototypes} prototypes + {src_matched} parsed calls + "
          f"{unaccounted} unaccounted")
    if unaccounted:
        print("  WARNING: unaccounted call sites -- the union below is a "
              "LOWER BOUND")
    out = {
        "reference_count": n,
        "src_occurrences": occurrences,
        "src_prototypes": prototypes,
        "src_parsed_calls": src_matched,
        "src_unaccounted": unaccounted,
        "sources": {},
    }
    for src, name, blob_size in ((SRC_REL, "foresta.rel", 15_640_056),
                                 (SRC_DOL, "main.dol", 918_720)):
        rs = sorted(ranges[src])
        merged: list[list[int]] = []
        for a, b in rs:
            if merged and a <= merged[-1][1]:
                merged[-1][1] = max(merged[-1][1], b)
            else:
                merged.append([a, b])
        union = sum(b - a for a, b in merged)
        raw = sum(b - a for a, b in rs)
        end = merged[-1][1] if merged else 0
        print(f"  {name:<14} {len(rs):>6} refs  raw {mib(raw):>10}  "
              f"union {mib(union):>10} in {len(merged)} spans  "
              f"max end 0x{end:X}")
        out["sources"][name] = {
            "references": len(rs), "raw_bytes": raw, "union_bytes": union,
            "spans": len(merged), "max_end": end, "blob_size_hint": blob_size,
        }
        if args.spans:
            out["sources"][name]["span_list"] = [[a, b] for a, b in merged]
    if args.json:
        Path(args.json).write_text(json.dumps(out, indent=2))
        print(f"json written: {args.json}")
    return 0


# ------------------------------------------------------------------- verify


def cmd_verify(args) -> int:
    root = Path(args.root).resolve()
    mpath = root / "manifest.json"
    if not mpath.exists():
        sys.exit(f"no manifest.json in {root}")
    man = json.loads(mpath.read_text())
    bad = 0
    for e in man["files"]:
        p = root / e["path"]
        if not p.exists():
            print(f"MISSING  {e['path']}")
            bad += 1
            continue
        if p.stat().st_size != e["size"]:
            print(f"SIZE     {e['path']}: {p.stat().st_size} != {e['size']}")
            bad += 1
            continue
        if sha256_file(p) != e["sha256"]:
            print(f"HASH     {e['path']}")
            bad += 1
    print(f"{len(man['files']) - bad}/{len(man['files'])} files ok "
          f"({mib(man['total_size'])})")
    return 1 if bad else 0


# ---------------------------------------------------------------------- cli


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        prog="dcasset",
        description="GameCube disc image -> Dreamcast disc layout")
    sub = ap.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("report", help="measure padding vs real content")
    r.add_argument("image")
    r.add_argument("--json", help="also write the full measurement as JSON")
    r.add_argument("--no-lzma", action="store_true",
                   help="skip the (slow) lzma measurement")
    r.add_argument("--force", action="store_true",
                   help="proceed on a non-GAFE01-rev0 image")
    r.set_defaults(func=cmd_report)

    e = sub.add_parser("extract", help="write a Dreamcast disc tree + manifest")
    e.add_argument("image")
    e.add_argument("--out", default=DEFAULT_OUT,
                   help=f"output directory (default: {DEFAULT_OUT})")
    e.add_argument("--keep-szs", action="store_true",
                   help="keep foresta.rel.szs Yaz0-compressed instead of "
                        "expanding it to foresta.rel")
    e.add_argument("--keep-sys", action="store_true",
                   help="also emit the GC system area (boot/bi2/apploader/fst)")
    e.add_argument("--force", action="store_true",
                   help="proceed on a non-GAFE01-rev0 image")
    e.set_defaults(func=cmd_extract)

    rm = sub.add_parser(
        "relmap",
        help="measure which REL/DOL bytes pc_assets.c actually references")
    rm.add_argument("--repo", default=str(REPO_ROOT),
                    help=f"repo root (default: {REPO_ROOT})")
    rm.add_argument("--json", help="write the span map as JSON")
    rm.add_argument("--spans", action="store_true",
                    help="include the full merged span list in --json")
    rm.set_defaults(func=cmd_relmap)

    v = sub.add_parser("verify", help="check an extracted tree against manifest")
    v.add_argument("root")
    v.set_defaults(func=cmd_verify)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
