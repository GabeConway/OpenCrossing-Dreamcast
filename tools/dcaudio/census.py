#!/usr/bin/env python3
"""Measure what an AICA offload would actually have to hold in sound RAM.

    python3 tools/dcaudio/census.py
    python3 tools/dcaudio/census.py --usable 1900544 --json out.json

`kb/audio-engine.md` §3.5 sizes the AICA offload by multiplying VADPCM byte
counts by 8/9.  That arithmetic is right but it is arithmetic, and the three
blockers it produces (a soundfont that does not fit, 24 samples that exceed a
hardware channel, 5.99 MB of samples against 1.9 MB of RAM) have never been
recomputed from the bank itself.  This does that, and prints the per-sequence
working sets that decide whether a residency manager is viable at all.

WHY 8/9 IS THE RIGHT RATIO, stated once so nobody re-derives it: VADPCM packs
16 samples into 9 bytes (one scale/predictor header plus 16 nibbles); AICA
ADPCM is a flat 4 bits per sample with no per-frame header, so the same 16
samples cost 8 bytes.  The saving is entirely the header byte.

⚠️ EVERY FIGURE HERE ASSUMES 4-BIT THROUGHOUT.  `kb/audio-engine.md` §3.5's
mitigation for the loop-discontinuity blocker is to store looping samples as
8-bit PCM instead, which DOUBLES them.  That interacts with the fit, so this
tool prints the all-4-bit number and the what-if-every-looping-sample-is-8-bit
number side by side.  The truth is somewhere between and only ears can say
where; do not quote the optimistic column alone.

⚠️ A SEQUENCE'S WORKING SET IS NOT ITS SOUNDFONT'S SIZE.  Sequences share
soundfonts and soundfonts share samples, so the union matters and the sum
double-counts.  Both are printed.
"""

from __future__ import annotations

import argparse
import json
import sys
from typing import Dict, List, Set, Tuple

try:
    from .audiorom import AudioRom, Sample
except ImportError:  # run as a script rather than a module
    import os
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from dcaudio.audiorom import AudioRom, Sample

# AICA total is 2 MB. KOS's ARM driver owns a region at the bottom; the rest is
# what snd_mem_malloc() can hand out. The default here is the figure
# kb/audio-engine.md §3.5 derives; override with --usable once it is confirmed
# against the KOS headers in the SDK image.
AICA_TOTAL = 2 * 1024 * 1024
DEFAULT_USABLE = 1_900_544

# An AICA channel addresses a 16-bit sample count. kb/audio-engine.md §3.5 uses
# 65,536; KOS's sfxmgr.h documents its own limit as 65,534. Both appear; the
# census reports against the conservative one and lists anything near it.
CHANNEL_LIMIT = 65534


def aica_bytes(n_samples: int, eight_bit: bool = False) -> int:
    """Sound-RAM cost of a decoded sample in AICA's own formats."""
    return n_samples if eight_bit else (n_samples + 1) // 2


def commas(n: int) -> str:
    return f"{n:,}"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--img", default=None, help="path to audiorom.img")
    ap.add_argument("--headers", default=None, help="path to audioheaders.c")
    ap.add_argument("--usable", type=int, default=DEFAULT_USABLE,
                    help=f"allocatable AICA sound RAM (default {DEFAULT_USABLE})")
    ap.add_argument("--json", default=None, help="also write the manifest here")
    ap.add_argument("--top", type=int, default=12,
                    help="how many worst-case sequences to list")
    args = ap.parse_args()

    kw = {}
    if args.img:
        kw["img_path"] = args.img
    if args.headers:
        kw["headers_path"] = args.headers
    rom = AudioRom(**kw)

    uniq: Dict[Tuple[int, int, int], Sample] = rom.unique_samples()
    samples = list(uniq.values())

    print("=" * 72)
    print("audiorom.img census")
    print("=" * 72)
    print(f"  extents          "
          + ", ".join(f"0x{a:X}+{commas(s)}" for a, s in rom.extents))
    print(f"  sequences        {len(rom.seqs)}"
          f"   ({sum(1 for e in rom.seqs if e.is_alias)} aliases)")
    print(f"  soundfonts       {len(rom.banks)}"
          f"   ({sum(1 for e in rom.banks if e.is_alias)} aliases)")
    print(f"  wave banks       {len(rom.waves)}")
    print(f"  unique samples   {len(samples)}")

    vadpcm_total = sum(s.size for s in samples)
    pcm_total = sum(s.n_samples for s in samples)
    a4 = sum(aica_bytes(s.n_samples) for s in samples)
    a8_loops = sum(
        aica_bytes(s.n_samples, eight_bit=s.loop.loops) for s in samples
    )
    looping = [s for s in samples if s.loop.loops]

    print()
    print(f"  VADPCM on disc   {commas(vadpcm_total)} B")
    print(f"  decoded          {commas(pcm_total)} samples "
          f"({commas(pcm_total * 2)} B as PCM16)")
    print(f"  as AICA 4-bit    {commas(a4)} B     <- everything, all 4-bit")
    print(f"  ...if the {len(looping)} looping samples go 8-bit: "
          f"{commas(a8_loops)} B")
    print(f"  looping {len(looping)}   one-shot {len(samples) - len(looping)}")

    # ---- blocker: the channel sample limit -------------------------------
    over = sorted((s for s in samples if s.n_samples > CHANNEL_LIMIT),
                  key=lambda s: -s.n_samples)
    print()
    print(f"  BLOCKER: samples over the {commas(CHANNEL_LIMIT)}-sample "
          f"channel limit: {len(over)}")
    if over:
        print(f"    longest {commas(over[0].n_samples)} samples "
              f"({over[0].n_samples / CHANNEL_LIMIT:.2f}x the limit), "
              f"in wave bank {over[0].wave_bank}")
        wb: Dict[int, int] = {}
        for s in over:
            wb[s.wave_bank] = wb.get(s.wave_bank, 0) + 1
        print("    by wave bank: "
              + ", ".join(f"wb{k}={v}" for k, v in sorted(wb.items())))
        print(f"    they are {commas(sum(aica_bytes(s.n_samples) for s in over))}"
              f" B of the {commas(a4)} B total")

    # ---- blocker: per-soundfont fit --------------------------------------
    print()
    print(f"  BLOCKER: soundfonts against {commas(args.usable)} B usable")
    bank_cost: List[Tuple[int, int, int]] = []
    for b in range(len(rom.banks)):
        ss = {s.key: s for s in rom.bank_samples(b)}
        cost = sum(aica_bytes(s.n_samples) for s in ss.values())
        bank_cost.append((b, cost, len(ss)))
    bank_cost.sort(key=lambda t: -t[1])
    oversize = [t for t in bank_cost if t[1] > args.usable]
    print(f"    {len(oversize)} soundfont(s) do not fit on their own")
    for b, cost, n in bank_cost[:6]:
        flag = "  <-- OVER" if cost > args.usable else ""
        print(f"      bank {b:3d}  {commas(cost):>10} B  {n:4d} samples"
              f"  {cost / args.usable * 100:5.1f}% of usable{flag}")

    # ---- the real unit of residency: the sequence ------------------------
    print()
    print("  RESIDENCY: per-sequence working sets (the union of its banks)")
    seq_cost: List[Tuple[int, int, int, List[int]]] = []
    bank_sample_keys: Dict[int, Set[Tuple[int, int, int]]] = {}
    for b in range(len(rom.banks)):
        bank_sample_keys[b] = {s.key for s in rom.bank_samples(b)}

    for q in range(len(rom.seqs)):
        try:
            banks = rom.seq_banks(q)
        except (IndexError, ValueError):
            continue
        keys: Set[Tuple[int, int, int]] = set()
        for b in banks:
            keys |= bank_sample_keys.get(b, set())
        cost = sum(aica_bytes(uniq[k].n_samples) for k in keys if k in uniq)
        seq_cost.append((q, cost, len(keys), banks))

    if seq_cost:
        costs = sorted(c for _, c, _, _ in seq_cost)
        median = costs[len(costs) // 2]
        mean = sum(costs) // len(costs)
        n_fit = sum(1 for c in costs if c <= args.usable)
        one_bank = sum(1 for _, _, _, b in seq_cost if len(b) == 1)
        print(f"    {len(seq_cost)} sequences   median {commas(median)} B"
              f"   mean {commas(mean)} B   max {commas(costs[-1])} B")
        print(f"    {one_bank}/{len(seq_cost)} reference exactly one soundfont")
        print(f"    {n_fit}/{len(seq_cost)} fit in {commas(args.usable)} B "
              f"on their own"
              f"  ({len(seq_cost) - n_fit} do NOT)")
        print()
        print(f"    worst {args.top}:")
        for q, cost, n, banks in sorted(seq_cost, key=lambda t: -t[1])[:args.top]:
            flag = "  <-- DOES NOT FIT" if cost > args.usable else ""
            print(f"      seq {q:3d}  {commas(cost):>10} B  {n:4d} samples"
                  f"  banks {banks}{flag}")

    print()
    print("  ⚠️ every figure above is 4-bit-throughout; see the module docstring")

    if args.json:
        manifest = {
            "usable_aica_bytes": args.usable,
            "channel_limit": CHANNEL_LIMIT,
            "totals": {
                "unique_samples": len(samples),
                "vadpcm_bytes": vadpcm_total,
                "decoded_samples": pcm_total,
                "aica4_bytes": a4,
                "aica4_with_8bit_loops_bytes": a8_loops,
                "looping": len(looping),
                "one_shot": len(samples) - len(looping),
            },
            "samples": [
                {
                    # device_addr == the raw file offset; this is the key the
                    # runtime recovers as (sampleAddr - GetNeosRomTop()).
                    "device_addr": s.file_off,
                    "wave_bank": s.wave_bank,
                    "vadpcm_bytes": s.size,
                    "n_samples": s.n_samples,
                    "aica4_bytes": aica_bytes(s.n_samples),
                    "loops": s.loop.loops,
                    "loop_start": s.loop.loop_start,
                    "loop_end": s.loop.loop_end,
                    "over_channel_limit": s.n_samples > CHANNEL_LIMIT,
                    "banks": sorted(s.banks),
                }
                for s in sorted(samples, key=lambda x: x.file_off)
            ],
            "sequences": [
                {"seq": q, "aica4_bytes": c, "n_samples": n, "banks": b}
                for q, c, n, b in seq_cost
            ],
        }
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(manifest, fh, indent=1)
        print(f"  manifest -> {args.json}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
