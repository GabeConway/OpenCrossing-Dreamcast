#!/usr/bin/env python3
"""Synthetic Animal Crossing save generator for the VMU budget benchmark.

This produces byte images with the EXACT struct layout of a real save
(save_layout.py, verified against the decomp headers) but SYNTHETIC contents.
It exists because no real GCI corpus was available on the dev machine; when a
real late-game .gci turns up, feed it to savebench.py --gci instead and the
synthetic numbers become a sanity check rather than the answer.

Fill profiles, least to most demanding:

  fresh        new town after the tutorial. Almost everything zero.
  typical      one well-played player, a season in. Partial fills.
  full         all 4 players maxed: every design/letter/diary/catalog slot in
               use, item grids near-full. Content is *plausible* — pixel art
               with flat regions, letters built from a shared word list.
  adversarial  same occupancy as `full`, but every user-authored byte is drawn
               uniformly at random (noise designs, random-character letters).
               This is the incompressible ceiling a player could actually
               author. It is not realistic; it is the number you size for if
               you refuse to ever corrupt a save.

Determinism: seeded PRNG, so re-running gives identical bytes.
"""
import random

import save_layout as L

# ---------------------------------------------------------------------------
# Content models
# ---------------------------------------------------------------------------

# AC stores text in a custom single-byte charset (see tools/msg_tool.py). We
# only need its statistics, not its exact code points: ~90 usable symbols,
# strings zero-terminated and zero-padded to the field width.
CHARSET_LO, CHARSET_HI = 0x20, 0x7A

WORDS = (
    "dear thanks for the letter i really liked it hope you are well come over "
    "sometime we should go fishing again that bug you gave me is on my wall "
    "the town is quiet today nook has a sale on furniture see you around your "
    "friend love from ps dont forget my birthday next week i moved the flowers "
    "by the river and planted a tree near the bridge did you catch the coelacanth "
    "it was raining all morning so i stayed inside listening to k k slider"
).split()

NAMES = ("bell nook tom able sable mabel blathers celeste redd gulliver kapp n "
         "tortimer copper booker pelly phyllis pete rover joan wisp "
         "olive bob rosie punchy stitches lucky ankha mitzi kiki tangy").split()

# Real GC item ids are 16-bit, heavily clustered by category (m_name_table.h).
ITEM_POOL = ([0x0000] * 4                       # EMPTY_NO dominates
             + list(range(0x1000, 0x1040))      # furniture-ish cluster
             + list(range(0x2200, 0x2220))      # tools cluster
             + list(range(0x3000, 0x3030)))     # misc cluster


def _text(rng, size, kind, fill, noise):
    """Zero-padded text field. `fill` = fraction of the field actually used."""
    if fill <= 0:
        return bytes(size)
    if kind == "name":
        used = size
        if noise:
            body = bytes(rng.randrange(CHARSET_LO, CHARSET_HI) for _ in range(used))
        else:
            body = (" ".join(rng.choice(NAMES) for _ in range(3))).encode()[:used]
    else:
        # body/line/diary: players rarely fill the whole field
        frac = {"line": 0.5, "body": 0.65, "diary": 0.55}.get(kind, 0.6)
        used = int(size * frac * min(fill, 1.0))
        if noise:
            body = bytes(rng.randrange(CHARSET_LO, CHARSET_HI) for _ in range(used))
        else:
            body = (" ".join(rng.choice(WORDS) for _ in range(used // 4 + 2))).encode()[:used]
    return body[:size] + bytes(size - len(body[:size]))


def _design(rng, size, fill, noise):
    """32x32 4bpp design texture (512 B) — the highest-entropy user data in AC."""
    assert size == 512
    if fill <= 0:
        return bytes(size)          # unused slot: cleared
    if noise:
        return bytes(rng.randrange(256) for _ in range(size))
    # Plausible hand-drawn pixel art: a background colour plus a handful of
    # blocks/lines in 3-5 other palette entries. Flat regions dominate, which
    # is what makes real designs compress at all.
    px = [rng.randrange(16)] * (32 * 32)
    pal = [rng.randrange(16) for _ in range(rng.randrange(3, 6))]
    for _ in range(rng.randrange(8, 20)):
        c = rng.choice(pal)
        x0, y0 = rng.randrange(32), rng.randrange(32)
        w, h = rng.randrange(2, 12), rng.randrange(2, 12)
        for y in range(y0, min(32, y0 + h)):
            for x in range(x0, min(32, x0 + w)):
                px[y * 32 + x] = c
    for _ in range(rng.randrange(20, 60)):      # speckle / outlining detail
        px[rng.randrange(1024)] = rng.choice(pal)
    return bytes((px[i] << 4) | px[i + 1] for i in range(0, 1024, 2))


def _itemgrid(rng, size, fill):
    out = bytearray(size)
    for i in range(0, size, 2):
        if rng.random() < fill:
            v = rng.choice(ITEM_POOL)
            out[i] = (v >> 8) & 0xFF
            out[i + 1] = v & 0xFF
    return bytes(out)


def _bitfield(rng, size, density, fill):
    d = density * fill
    out = bytearray(size)
    for i in range(size):
        b = 0
        for bit in range(8):
            if rng.random() < d:
                b |= 1 << bit
        out[i] = b
    return bytes(out)


def _misc(rng, size, fill):
    """Small mixed scalar fields: ids, timestamps, counters, enums.
    Modelled as mostly-small values with occasional full-range bytes."""
    out = bytearray(size)
    for i in range(size):
        if rng.random() < fill:
            out[i] = rng.randrange(64) if rng.random() < 0.7 else rng.randrange(256)
    return bytes(out)


def _sparse(rng, size, fill):
    """Quest/errand slots: usually entirely empty, occasionally populated."""
    out = bytearray(size)
    for i in range(0, size, 8):
        if rng.random() < fill * 0.25:
            for j in range(i, min(size, i + 8)):
                out[j] = rng.randrange(256) if rng.random() < 0.5 else 0
    return bytes(out)


PROFILES = {
    #                    fill  noise  design_slots_used
    "fresh":            (0.03, False, 0.03),
    "typical":          (0.35, False, 0.35),
    "full":             (0.95, False, 1.00),
    "adversarial":      (1.00, True,  1.00),
}


def generate(regions, total_size, profile, seed=0xAC0F):
    """Render a region tree into a byte image under the given fill profile."""
    fill, noise, design_fill = PROFILES[profile]
    rng = random.Random(seed)
    buf = bytearray(total_size)
    for off, size, group, kind, params in L.validate(regions, total_size, "gen"):
        if kind == "zero":
            continue
        elif kind == "text":
            data = _text(rng, size, params.get("kind", "line"), fill, noise)
        elif kind == "design4bpp":
            use = design_fill if rng.random() < design_fill else 0.0
            data = _design(rng, size, use, noise)
        elif kind == "itemgrid":
            data = _itemgrid(rng, size, fill * 0.9)
        elif kind == "itemid":
            data = _itemgrid(rng, size, fill)
        elif kind == "bitfield":
            data = _bitfield(rng, size, params.get("density", 0.5), fill)
        elif kind == "sparse":
            data = _sparse(rng, size, fill)
        elif kind == "misc":
            data = _misc(rng, size, fill)
        else:
            raise ValueError(kind)
        buf[off:off + size] = data
    return bytes(buf)


def generate_all(profile, seed=0xAC0F):
    """Return {name: bytes} for the DC-relevant unique payload blocks."""
    return {
        "Save_t":        generate(L.SAVE_T, L.SIZEOF["Save_t"], profile, seed),
        "keep_original": generate(L.KEEP_ORIGINAL, L.SIZEOF["mCD_keep_original_c"], profile, seed + 1),
        "keep_mail":     generate(L.KEEP_MAIL, L.SIZEOF["mCD_keep_mail_c"], profile, seed + 2),
        "keep_diary":    generate(L.KEEP_DIARY, L.SIZEOF["mCD_keep_diary_c"], profile, seed + 3),
    }


def group_slices(regions, total_size, image):
    """Split an image into per-group byte strings (for entropy attribution)."""
    groups = {}
    for off, size, group, kind, _ in L.validate(regions, total_size, "slice"):
        groups.setdefault(group, bytearray()).extend(image[off:off + size])
    return {k: bytes(v) for k, v in groups.items()}


if __name__ == "__main__":
    import sys
    prof = sys.argv[1] if len(sys.argv) > 1 else "full"
    blocks = generate_all(prof)
    for name, data in blocks.items():
        nz = sum(1 for b in data if b)
        print(f"{name:16s} {len(data):8d} bytes, {nz * 100 // len(data):3d}% non-zero")
