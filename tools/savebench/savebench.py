#!/usr/bin/env python3
"""Measure whether an Animal Crossing save fits on a Dreamcast VMU.

Usage:
  python3 savebench.py                     # all synthetic profiles, all codecs
  python3 savebench.py --profile full      # one profile
  python3 savebench.py --groups full       # per-field-group attribution
  python3 savebench.py --gci PATH.gci ...  # REAL saves (preferred evidence)
  python3 savebench.py --trim full         # PLAN.md 6(c): content trimming
  python3 savebench.py --delta full        # PLAN.md 6(d): base + journal
  python3 savebench.py --dict full         # CD-resident preset dictionary
  python3 savebench.py --cost full         # compression + VMU write time
  python3 savebench.py --recommend         # the shipping proposal

Re-run this whenever Save_t or the keep-blocks change; re-run save_layout_probe.c
first if the struct definitions moved (see README.md).
"""
import argparse
import bz2
import lzma
import sys
import zlib

import save_layout as L
import gen_save

# ---------------------------------------------------------------------------
# Codecs. Only ones that could plausibly run on SH-4 @ 200 MHz in <= ~1 s for
# a few hundred KB are marked dc_viable; the rest are reference points.
# ---------------------------------------------------------------------------


def _deflate(level):
    def f(b):
        c = zlib.compressobj(level, zlib.DEFLATED, -15)
        return c.compress(b) + c.flush()
    return f


def _lzma(preset):
    def f(b):
        return lzma.compress(b, format=lzma.FORMAT_RAW,
                             filters=[{"id": lzma.FILTER_LZMA2, "preset": preset}])
    return f


CODECS = [
    # name           fn                    dc_viable  note
    ("store",        lambda b: b,          True,  "no compression"),
    ("deflate-1",    _deflate(1),          True,  "zlib, kos-ports"),
    ("deflate-6",    _deflate(6),          True,  "zlib, kos-ports"),
    ("deflate-9",    _deflate(9),          True,  "zlib, kos-ports"),
    ("bzip2-9",      lambda b: bz2.compress(b, 9), False, "BWT, too slow/RAM-hungry"),
    ("lzma-6",       _lzma(6),             False, "reference ceiling"),
    ("lzma-9e",      _lzma(9 | lzma.PRESET_EXTREME), False, "reference ceiling"),
]

try:
    import zstandard as _zstd  # optional

    def _zst(level):
        return lambda b: _zstd.ZstdCompressor(level=level).compress(b)
    CODECS[4:4] = [("zstd-1", _zst(1), True, "needs ~1 MB ctx on DC"),
                   ("zstd-9", _zst(9), False, ""),
                   ("zstd-19", _zst(19), False, "")]
except ImportError:
    pass


def fmt_verdict(payload, label=""):
    """Report a compressed payload against the VMU budget."""
    hdr = L.vms_header_bytes(icons=1, eyecatch=0)
    blocks = L.blocks_for(payload)
    fits1 = blocks <= L.VMU_USER_BLOCKS
    fits2 = blocks <= 2 * L.VMU_USER_BLOCKS
    v = "1 VMU" if fits1 else ("2 VMUs" if fits2 else "DOES NOT FIT 2 VMUs")
    return (f"{payload:8d} B + {hdr} B hdr = {blocks:4d} blocks "
            f"({blocks / L.VMU_USER_BLOCKS * 100:5.1f}% of one VMU) -> {v} {label}")


def bench(images, title):
    total_raw = sum(len(v) for v in images.values())
    print(f"\n=== {title} ===")
    print(f"uncompressed unique payload: {total_raw} bytes "
          f"({total_raw / 1024:.1f} KiB)")
    print(f"VMU user area: {L.VMU_USER_BYTES} B / {L.VMU_USER_BLOCKS} blocks; "
          f"required ratio for one VMU: "
          f"{total_raw / (L.VMU_USER_BYTES - L.vms_header_bytes()):.2f}:1")
    print()
    print(f"  {'codec':<12} {'DC?':<4} {'bytes':>9} {'ratio':>7} {'blocks':>7}  verdict")
    blob = b"".join(images[k] for k in sorted(images))
    for name, fn, viable, note in CODECS:
        out = fn(blob)
        blocks = L.blocks_for(len(out))
        v = ("FITS 1 VMU" if blocks <= L.VMU_USER_BLOCKS else
             "fits 2 VMUs" if blocks <= 2 * L.VMU_USER_BLOCKS else "NO FIT")
        print(f"  {name:<12} {'yes' if viable else 'no':<4} {len(out):>9} "
              f"{total_raw / len(out):>6.2f}x {blocks:>7}  {v}"
              + (f"   ({note})" if note else ""))
    return blob


def bench_groups(profile):
    """Per-field-group compressibility — shows WHICH parts of the save cost."""
    print(f"\n=== per-group attribution, profile={profile} ===")
    print(f"  {'group':<16} {'raw B':>8} {'defl-9':>8} {'ratio':>7} {'lzma':>8} "
          f"{'ratio':>7}  {'% of deflated total':>20}")
    specs = [
        (L.SAVE_T, L.SIZEOF["Save_t"], "Save_t"),
        (L.KEEP_ORIGINAL, L.SIZEOF["mCD_keep_original_c"], "keep_original"),
        (L.KEEP_MAIL, L.SIZEOF["mCD_keep_mail_c"], "keep_mail"),
        (L.KEEP_DIARY, L.SIZEOF["mCD_keep_diary_c"], "keep_diary"),
    ]
    groups = {}
    for regions, size, name in specs:
        img = gen_save.generate(regions, size, profile,
                                seed=0xAC0F + specs.index((regions, size, name)))
        for g, data in gen_save.group_slices(regions, size, img).items():
            groups.setdefault(g, bytearray()).extend(data)
    d9, lz = _deflate(9), _lzma(9 | lzma.PRESET_EXTREME)
    rows = []
    for g, data in groups.items():
        data = bytes(data)
        rows.append((g, len(data), len(d9(data)), len(lz(data))))
    tot_d = sum(r[2] for r in rows)
    for g, raw, cd, cl in sorted(rows, key=lambda r: -r[2]):
        print(f"  {g:<16} {raw:>8} {cd:>8} {raw / cd:>6.2f}x {cl:>8} "
              f"{raw / cl:>6.2f}x  {cd * 100 / tot_d:>19.1f}%")
    print(f"  {'TOTAL (sep.)':<16} {sum(r[1] for r in rows):>8} {tot_d:>8}")
    print("  note: groups compressed separately, so this over-counts vs the "
          "single-stream figure above (no cross-group matches).")


def bench_trim(profile="adversarial"):
    """Evaluate PLAN.md §6 option (c): trimming save content."""
    print(f"\n=== trim options (profile={profile}) ===")
    imgs = gen_save.generate_all(profile)
    d = _deflate(9)
    ko, km, kd = imgs["keep_original"], imgs["keep_mail"], imgs["keep_diary"]
    ko2 = ko[:128 + 24 * 544]            # 2 pages of 12 designs (96 -> 24)
    km2 = km[:100 + 40 * 298]            # 2 pages of 20 letters (160 -> 40)
    kd1 = kd[:2 + 12 * 992]              # 1 player-year of diary (48 -> 12)
    variants = [
        ("full save (nothing trimmed)", [imgs["Save_t"], ko, km, kd]),
        ("drop keep_diary", [imgs["Save_t"], ko, km]),
        ("drop keep_diary + keep_mail", [imgs["Save_t"], ko]),
        ("lockers 8 pages -> 2, diary 1 yr", [imgs["Save_t"], ko2, km2, kd1]),
        ("... and no diary at all", [imgs["Save_t"], ko2, km2]),
        ("drop all three keep-blocks", [imgs["Save_t"]]),
    ]
    for label, parts in variants:
        blob = b"".join(parts)
        out = d(blob)
        print(f"  {label:<34} raw {len(blob):7d} -> defl {len(out):6d}  "
              + fmt_verdict(len(out)))


def bench_delta(profile="full", seed=0xAC0F):
    """Evaluate PLAN.md §6 option (d): base image + per-session change log.

    Models 'one more day of play' as a perturbation of a small fraction of the
    mutable regions, then measures the compressed size of (a) the whole new
    image and (b) the 512-byte-granular diff against the previous image.
    """
    print(f"\n=== delta / journal (profile={profile}) ===")
    base = b"".join(gen_save.generate_all(profile, seed)[k]
                    for k in sorted(gen_save.generate_all(profile, seed)))
    d = _deflate(9)
    print(f"  base image {len(base)} B -> deflate-9 {len(d(base))} B "
          f"({L.blocks_for(len(d(base)))} blocks)")
    import random
    for churn in (0.005, 0.02, 0.10):
        rng = random.Random(1234)
        nxt = bytearray(base)
        for i in range(0, len(nxt), 512):          # perturb whole 512 B pages
            if rng.random() < churn:
                for j in range(i, min(len(nxt), i + 512)):
                    if rng.random() < 0.25:
                        nxt[j] = rng.randrange(256)
        nxt = bytes(nxt)
        changed = sum(1 for i in range(0, len(base), 512)
                      if base[i:i + 512] != nxt[i:i + 512])
        xor = bytes(a ^ b for a, b in zip(base, nxt))
        print(f"  churn {churn * 100:4.1f}%: {changed:4d}/{len(base) // 512} pages dirty; "
              f"full re-deflate {len(d(nxt)):6d} B; "
              f"dirty pages raw {changed * 512:6d} B -> deflate {len(d(b''.join(nxt[i:i+512] for i in range(0, len(nxt), 512) if base[i:i+512] != nxt[i:i+512]))):6d} B; "
              f"XOR-delta deflate {len(d(xor)):6d} B")
    print("  -> a journal bounds WRITE COST, not FOOTPRINT: the base image still "
          "has to live on the VMU in full.")


def bench_dict(profile="full"):
    """Does a CD-resident preset dictionary buy anything? (zlib caps it at 32 KB.)"""
    print(f"\n=== deflate preset dictionary (profile={profile}) ===")
    imgs = gen_save.generate_all(profile)
    blob = b"".join(imgs[k] for k in sorted(imgs))
    plain = len(_deflate(9)(blob))
    fresh = gen_save.generate_all("fresh")
    freshblob = b"".join(fresh[k] for k in sorted(fresh))
    for dsize in (8192, 32768):
        dic = freshblob[-dsize:]
        c = zlib.compressobj(9, zlib.DEFLATED, -15, 9, 0, zdict=dic)
        out = c.compress(blob) + c.flush()
        print(f"  {dsize:6d} B dictionary (tail of a fresh save): "
              f"{len(out):7d} B vs {plain} B plain  "
              f"({(plain - len(out)) * 100 / plain:+.2f}%)")


def bench_cost(profile="full"):
    """Write cost: compression time + VMU flash write time for the payload.

    Host deflate throughput is measured; the SH-4 figure is an EXTRAPOLATION
    (scale factor SH4_SCALE below) and must be replaced with a Flycast/hardware
    measurement at M3. The VMU numbers bracket the two known constraints.
    """
    import time
    SH4_SCALE = (40, 100)     # host-core / SH-4@200MHz scalar throughput ratio
    imgs = gen_save.generate_all(profile)
    blob = b"".join(imgs[k] for k in sorted(imgs))
    print(f"\n=== write cost (profile={profile}) ===")
    for lvl in (1, 6, 9):
        t0 = time.perf_counter()
        for _ in range(5):
            out = _deflate(lvl)(blob)
        dt = (time.perf_counter() - t0) / 5
        mbs = len(blob) / dt / 1e6
        lo, hi = [dt * s for s in SH4_SCALE]
        print(f"  deflate-{lvl}: host {dt * 1000:6.1f} ms ({mbs:5.1f} MB/s), "
              f"-> {len(out):6d} B; SH-4 extrapolation {lo:5.2f}-{hi:5.2f} s "
              f"(UNVERIFIED)")
    out = _deflate(6)(blob)
    blocks = L.blocks_for(len(out))
    print(f"\n  VMU write of {blocks} blocks ({len(out)} B payload + header):")
    # bound A: VMU flash floor, ~10 ms per block write (dreamcast.wiki)
    # bound B: KOS does 5 dependent maple transactions per block
    #          (4 x 128 B phases + BSYNC); if each costs one ~16.7 ms maple
    #          DMA cycle that is ~83 ms/block.
    print(f"    optimistic (10 ms/block, flash floor)     : {blocks * 0.010:6.2f} s")
    print(f"    pessimistic (5 maple cycles @16.7 ms/blk) : {blocks * 5 * 0.0167:6.2f} s")
    print("    -> MEASURE on hardware; the true value is somewhere between.")


CHUNK = 16384


def chunked(blob, level=6, chunk=CHUNK):
    """Independently-deflated fixed-size chunks + a 2-byte length per chunk.

    Costs a few percent of ratio and buys byte-stable output for unchanged
    chunks, which is what makes incremental (dirty-block-only) VMU writes
    possible — a whole-stream deflate shifts every byte after an edit.
    """
    parts = [_deflate(level)(blob[i:i + chunk]) for i in range(0, len(blob), chunk)]
    return sum(len(p) for p in parts) + 2 * len(parts), len(parts)


def trim_dc_edition(imgs, designs=24, letters=40, diary=12):
    """The proposed DC-edition storage-locker trim (see kb/save-budget.md §6)."""
    return (imgs["Save_t"]
            + imgs["keep_original"][:128 + designs * L.SIZEOF["mNW_original_design_c"]]
            + imgs["keep_mail"][:100 + letters * L.SIZEOF["Mail_c"]]
            + imgs["keep_diary"][:2 + diary * L.SIZEOF["mDi_entry_c"]])


def bench_recommend():
    """The shipping proposal, measured across all profiles."""
    print("\n=== RECOMMENDED CONFIG: DC-edition locker trim + chunked deflate-6 ===")
    print(f"  chunk size {CHUNK} B; lockers 96->24 designs, 160->40 letters, "
          f"48->12 diary entries")
    print(f"  {'profile':<14} {'variant':<26} {'raw':>8} {'whole':>8} {'chunked':>8} "
          f"{'blocks':>7}  {'% VMU':>7}")
    for prof in gen_save.PROFILES:
        imgs = gen_save.generate_all(prof)
        for label, blob in (("untrimmed", b"".join(imgs[k] for k in sorted(imgs))),
                            ("DC-edition trim", trim_dc_edition(imgs))):
            whole = len(_deflate(6)(blob))
            ch, n = chunked(blob)
            blocks = L.blocks_for(ch)
            print(f"  {prof:<14} {label:<26} {len(blob):>8} {whole:>8} {ch:>8} "
                  f"{blocks:>7}  {blocks / L.VMU_USER_BLOCKS * 100:>6.1f}%"
                  + ("" if blocks <= L.VMU_USER_BLOCKS else "  OVER BUDGET"))


def load_gci(path):
    """Extract the unique payload out of a real DobutsunomoriP_MURA.gci."""
    data = open(path, "rb").read()
    want = L.SIZEOF["GCI_HEADER"] + L.SIZEOF["mCD_LAND_SAVE_SIZE"]
    if len(data) != want:
        print(f"  WARNING: {path} is {len(data)} B, expected {want}", file=sys.stderr)
    base = L.SIZEOF["GCI_HEADER"]
    others = base                                   # GCI_OTHERS_OFFSET  = 0
    main = base + L.SIZEOF["OTHERS_SIZE"]           # GCI_SAVE_MAIN_OFFSET
    # keep-blocks sit after MemcardHeader_c (5184) + 32 B pad inside others
    ko = others + 5216
    km = ko + L.SIZEOF["mCD_keep_original_c"]
    kd = km + L.SIZEOF["mCD_keep_mail_c"]
    return {
        "Save_t":        data[main:main + L.SIZEOF["Save_t"]],
        "keep_original": data[ko:ko + L.SIZEOF["mCD_keep_original_c"]],
        "keep_mail":     data[km:km + L.SIZEOF["mCD_keep_mail_c"]],
        "keep_diary":    data[kd:kd + L.SIZEOF["mCD_keep_diary_c"]],
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--profile", choices=list(gen_save.PROFILES))
    ap.add_argument("--groups", metavar="PROFILE")
    ap.add_argument("--gci", nargs="+", metavar="PATH")
    ap.add_argument("--trim", nargs="?", const="adversarial", metavar="PROFILE")
    ap.add_argument("--delta", nargs="?", const="full", metavar="PROFILE")
    ap.add_argument("--dict", nargs="?", const="full", metavar="PROFILE",
                    dest="dictionary")
    ap.add_argument("--cost", nargs="?", const="full", metavar="PROFILE")
    ap.add_argument("--recommend", action="store_true")
    a = ap.parse_args()

    print(f"layout: sizeof(Save_t)={L.SIZEOF['Save_t']}  "
          f"keep_original={L.SIZEOF['mCD_keep_original_c']}  "
          f"keep_mail={L.SIZEOF['mCD_keep_mail_c']}  "
          f"keep_diary={L.SIZEOF['mCD_keep_diary_c']}")
    print(f"VMU: {L.VMU_USER_BLOCKS} x {L.VMU_BLOCK} B = {L.VMU_USER_BYTES} B user area; "
          f"VMS header (1 icon, no eyecatch) = {L.vms_header_bytes()} B")

    if a.gci:
        for p in a.gci:
            bench(load_gci(p), f"REAL SAVE {p}")
        return
    if a.groups:
        bench_groups(a.groups)
        return
    if a.trim:
        bench_trim(a.trim)
        return
    if a.delta:
        bench_delta(a.delta)
        return
    if a.dictionary:
        bench_dict(a.dictionary)
        return
    if a.cost:
        bench_cost(a.cost)
        return
    if a.recommend:
        bench_recommend()
        return
    profiles = [a.profile] if a.profile else list(gen_save.PROFILES)
    for p in profiles:
        bench(gen_save.generate_all(p), f"SYNTHETIC profile={p}")


if __name__ == "__main__":
    main()
