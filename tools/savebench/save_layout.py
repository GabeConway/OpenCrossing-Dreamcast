#!/usr/bin/env python3
"""Animal Crossing (GAFE01) save layout — ground truth for the DC VMU budget.

Every offset/size here was VERIFIED by compiling `save_layout_probe.c` against
the vendored decomp headers for a 32-bit ARM EABI target (same alignment rules
as the GameCube PPC ABI and as SH-4 with -m4-single-only): see
tools/savebench/README.md for the exact docker command. The probe's offsets
reproduce the `/* 0x...... */` comments in include/m_common_data.h byte for
byte, which is the cross-check that the model is right.

Key totals (bytes):
  sizeof(Save_t)              148128   0x242A0   include/m_common_data.h:81-174
  sizeof(Save) (sector-align) 155648   0x26000   include/m_common_data.h:176-179
  sizeof(mCD_others_c)        153056   0x255E0   include/m_card.h:179-185
  OTHERS_SIZE (sector-align)  155648   0x26000   include/m_card.h:287
  mCD_LAND_SAVE_SIZE          466944   0x72000   include/m_card.h:17  (57 x 8 KB)

GCI file layout written by the port (pc/src/pc_m_card.c:56-61):
  0x000000  64      CARDDir header
  0x000040  0x26000 mCD_others_c   (pattern/letter/diary lockers + banner+icon)
  0x026040  0x26000 Save (main)
  0x04C040  0x26000 Save (backup, byte-identical duplicate)
  total 467008 bytes on disk / 466944 on card / 57 memory-card blocks.
"""

# ---------------------------------------------------------------------------
# Sizes verified by save_layout_probe.c (armhf gcc, Debian bookworm)
# ---------------------------------------------------------------------------
SIZEOF = {
    "Save_t":                148128,   # 0x242A0
    "Save":                  155648,   # 0x26000 (sector aligned)
    "common_data_t":         187392,   # 0x2DC00
    "mCD_others_c":          153056,   # 0x255E0
    "OTHERS_SIZE":           155648,   # 0x26000
    "mCD_keep_original_c":    52384,   # 0xCCA0
    "mCD_keep_mail_c":        47780,   # 0xBAA4
    "mCD_keep_diary_c":       47618,   # 0xBA02
    "MemcardHeader_c":         5184,   # 0x1440 comment+banner+icon
    "Private_c":               9280,   # 0x2440
    "mHm_hs_c":                9904,   # 0x26B0
    "mHm_flr_c":               2216,   # 0x8A8
    "mHm_lyr_c":                552,   # 0x228
    "mHm_cottage_c":           2248,   # 0x8C8
    "Animal_c":                2440,   # 0x988
    "Anmmem_c":                 312,   # 0x138
    "Mail_c":                   298,   # 0x12A
    "Island_c":                6400,   # 0x1900
    "mNW_needlework_c":        4352,   # 0x1100  (8 designs)
    "mNW_original_design_c":    544,   # 0x220
    "mDi_entry_c":              992,   # 0x3E0
    "mFM_fg_c":                 512,   # 0x200
    "mNtc_board_post_c":        200,   # 0xC8
    "mCD_LAND_SAVE_SIZE":    466944,   # 0x72000
    "GCI_HEADER":                64,
}

COUNTS = {
    "PLAYER_NUM":            4,
    "ANIMAL_NUM_MAX":       15,
    "FG_ACRES":             30,   # FG_BLOCK_Z_NUM 6 x FG_BLOCK_X_NUM 5
    "keep_original_designs": 96,  # 8 pages x 12
    "keep_mail_letters":    160,  # 8 pages x 20
    "keep_diary_entries":    48,  # 4 players x 12 months
}

# ---------------------------------------------------------------------------
# Region model.  Each entry: (offset, size, group, kind, params)
#   group : the reporting bucket (what the kb table shows)
#   kind  : how the synthetic generator fills it
# Regions must tile the struct exactly with no gaps and no overlap; anything
# not modelled explicitly is declared as a "zero" region (real saves genuinely
# zero their padding — pc_m_card.c:262 callocs the whole file image).
# ---------------------------------------------------------------------------

# --- leaf composites (relative offsets) ---

# mNW_original_design_c, include/m_needlework.h:56-61
DESIGN = [
    (0x000, 16,  "designs", "text",   {"kind": "name"}),
    (0x010, 16,  "designs", "misc",   {}),          # palette, flag, pad to 0x20
    (0x020, 512, "designs", "design4bpp", {}),      # 32x32 4bpp art
]

# Mail_c, include/m_mail.h:99-104
MAIL = [
    (0x000, 44,  "letters", "text",  {"kind": "name"}),   # Mail_hdr_c: 2 x PersonalID
    (0x02C, 2,   "letters", "itemid", {}),                # present
    (0x02E, 4,   "letters", "misc",  {}),                 # font/type/paper
    (0x032, 24,  "letters", "text",  {"kind": "line"}),   # header
    (0x04A, 192, "letters", "text",  {"kind": "body"}),   # body
    (0x10A, 32,  "letters", "text",  {"kind": "line"}),   # footer
]

# mHm_lyr_c, include/m_home_h.h:132-136  (one furniture layer of a room)
HOME_LAYER = [
    (0x000, 512, "house_layout", "itemgrid", {}),   # 16x16 u16 furniture ids
    (0x200, 8,   "house_layout", "bitfield", {"density": 0.3}),
    (0x208, 32,  "house_layout", "misc", {}),
]

# mHm_flr_c, include/m_home_h.h:143-151  (one room = 4 layers)
HOME_FLOOR = (
    [(0x000 + i * 552, None, None, "sub", {"sub": HOME_LAYER}) for i in range(4)]
    + [(0x8A0, 8, "house_layout", "misc", {})]
)

# mHm_cottage_c, include/m_home_h.h:180-188
COTTAGE = [
    (0x000, 8,    "house_layout", "misc", {}),
    (0x008, None, None, "sub", {"sub": HOME_FLOOR}),
    (0x8B0, 12,   "house_layout", "misc", {}),
    (0x8BC, 8,    "house_layout", "bitfield", {"density": 0.2}),
    (0x8C4, 4,    "house_layout", "zero", {}),
]

# mHm_hs_c, include/m_home_h.h:154-177
HOME = (
    [
        (0x0000, 20, "house_misc", "text", {"kind": "name"}),   # ownerID
        (0x0014, 20, "house_misc", "misc", {}),                 # ..0x28
        (0x0028, 16, "house_misc", "misc", {}),                 # ..0x38
    ]
    + [(0x0038 + i * 2216, None, None, "sub", {"sub": HOME_FLOOR}) for i in range(3)]
    + [(0x1A30 + i * 298, None, None, "sub", {"sub": MAIL}) for i in range(10)]
    + [
        (0x25D4, 32,  "house_misc", "misc", {}),                # haniwa held items
        (0x25F4, 128, "house_misc", "text", {"kind": "line"}),  # haniwa message
        (0x2674, 16,  "house_misc", "misc", {}),
        (0x2684, 8,   "house_misc", "bitfield", {"density": 0.3}),  # music_box
        (0x268C, 36,  "house_misc", "zero", {}),
    ]
)

# Anmmem_c, include/m_npc.h:177-186 (one villager's memory of one player)
ANIMAL_MEM = [
    (0x000, 20,  "villagers", "text", {"kind": "name"}),
    (0x014, 24,  "villagers", "misc", {}),
    (0x02C, 6,   "villagers", "misc", {}),
    (0x032, 6,   "villagers", "misc", {}),                     # Anmplmail_c header
    (0x038, 256, "villagers", "text", {"kind": "body"}),       # saved letter text
]

# Animal_c, include/m_npc.h:200-230
ANIMAL = (
    [(0x000, 16, "villagers", "misc", {})]
    + [(0x010 + i * 312, None, None, "sub", {"sub": ANIMAL_MEM}) for i in range(7)]
    + [
        (0x898, 5,   "villagers", "misc", {}),
        (0x89D, 11,  "villagers", "text", {"kind": "name"}),   # catchphrase
        (0x8A8, 40,  "villagers", "misc", {}),
        (0x8D0, 16,  "villagers", "text", {"kind": "name"}),
        (0x8E0, 16,  "villagers", "misc", {}),
        (0x8F0, 16,  "villagers", "misc", {}),                 # animal_relations
        (0x900, 112, "villagers", "misc", {}),                 # hp_mail passwords
        (0x970, 24,  "villagers", "zero", {}),
    ]
)

# Private_c, include/m_private.h:187-264
PRIVATE = (
    [
        (0x0000, 20,  "player_misc", "text", {"kind": "name"}),
        (0x0014, 4,   "player_misc", "misc", {}),
        (0x0018, 78,  "player_misc", "bitfield", {"density": 0.5}),  # museum_record
        (0x0066, 2,   "player_misc", "zero", {}),
        (0x0068, 30,  "player_items", "itemid", {}),                 # pockets
        (0x0086, 14,  "player_items", "misc", {}),
        (0x0094, 600, "player_misc", "sparse", {}),                  # deliveries
        (0x02EC, 440, "player_misc", "sparse", {}),                  # errands
        (0x04A4, 2,   "player_items", "itemid", {}),
        (0x04A6, 58,  "letters", "text", {"kind": "line"}),          # saved header
    ]
    + [(0x04E0 + i * 298, None, None, "sub", {"sub": MAIL}) for i in range(10)]
    + [
        (0x1084, 24,  "player_misc", "misc", {}),
        (0x109C, 8,   "player_misc", "misc", {}),
        (0x10A4, 24,  "player_misc", "misc", {}),
        (0x10BC, 24,  "player_misc", "zero", {}),
        (0x10D4, 8,   "catalog", "bitfield", {"density": 0.5}),      # aircheck
        (0x10DC, 24,  "player_misc", "misc", {}),
        (0x10F4, 4,   "player_misc", "misc", {}),
        (0x10F8, 16,  "player_misc", "text", {"kind": "name"}),
        (0x1108, 172, "catalog", "bitfield", {"density": 0.8}),      # furniture
        (0x11B4, 12,  "catalog", "bitfield", {"density": 0.8}),      # wallpaper
        (0x11C0, 12,  "catalog", "bitfield", {"density": 0.8}),      # carpet
        (0x11CC, 8,   "catalog", "bitfield", {"density": 0.8}),      # paper
        (0x11D4, 8,   "catalog", "bitfield", {"density": 0.8}),      # music
        (0x11DC, 80,  "player_misc", "text", {"kind": "name"}),      # foreign maps
        (0x122C, 20,  "player_misc", "misc", {}),
    ]
    + [(0x1240 + i * 544, None, None, "sub", {"sub": DESIGN}) for i in range(8)]
    + [
        (0x2340, 12,  "player_misc", "misc", {}),
        (0x234C, 104, "player_misc", "misc", {}),                    # calendar
        (0x23B4, 22,  "player_misc", "misc", {}),
        (0x23CA, 14,  "player_misc", "zero", {}),
        (0x23D8, 8,   "player_misc", "misc", {}),
        (0x23E0, 50,  "player_misc", "bitfield", {"density": 0.3}),  # e-Card letters
        (0x2412, 46,  "player_misc", "zero", {}),
    ]
)

# Island_c, include/m_island.h:70-85
ISLAND = (
    [
        (0x0000, 20,   "island", "text", {"kind": "name"}),
        (0x0014, 1024, "island", "itemgrid", {}),
        (0x0414, 4,    "island", "zero", {}),
        (0x0418, None, None, "sub", {"sub": COTTAGE}),
        (0x0CE0, None, None, "sub", {"sub": DESIGN}),      # island flag design
        (0x0F00, None, None, "sub", {"sub": ANIMAL}),      # islander
        (0x1888, 64,   "island", "bitfield", {"density": 0.15}),
        (0x18C8, 10,   "island", "misc", {}),
        (0x18D2, 14,   "island", "zero", {}),
        (0x18E0, 3,    "island", "misc", {}),
        (0x18E3, 29,   "island", "zero", {}),
    ]
)

# --- Save_t, include/m_common_data.h:81-174 ---
SAVE_T = (
    [
        (0x000000, 20,   "header",       "misc", {}),          # mFRm_chk_t
        (0x000014, 12,   "header",       "misc", {}),
    ]
    + [(0x000020 + i * 9280, None, None, "sub", {"sub": PRIVATE}) for i in range(4)]
    + [
        (0x009120, 12,   "town_misc",    "text", {"kind": "name"}),   # land_info
    ]
    + [(0x00912C + i * 200, 200, "noticeboard", "text", {"kind": "body"}) for i in range(15)]
    + [
        (0x009CE4, 4,     "town_misc",   "zero", {}),
    ]
    + [(0x009CE8 + i * 9904, None, None, "sub", {"sub": HOME}) for i in range(4)]
    + [
        (0x0137A8, 15360, "town_items",  "itemgrid", {}),      # fg[6][5]
        (0x0173A8, 140,   "town_misc",   "misc", {}),          # acre combi table
        (0x017434, 4,     "town_misc",   "zero", {}),
    ]
    + [(0x017438 + i * 2440, None, None, "sub", {"sub": ANIMAL}) for i in range(15)]
    + [
        (0x020330, 16,    "town_misc",   "misc", {}),
        (0x020340, 320,   "town_misc",   "misc", {}),          # shop
        (0x020480, 24,    "town_misc",   "misc", {}),          # kabu
        (0x020498, 188,   "town_misc",   "misc", {}),          # event save
        (0x020554, 308,   "town_misc",   "misc", {}),          # event common
        (0x020688, 12,    "town_misc",   "misc", {}),
        (0x020694, 2108,  "town_misc",   "sparse", {}),        # post office
        (0x020ED0, 40,    "town_misc",   "misc", {}),          # police box
        (0x020EF8, 24,    "town_misc",   "misc", {}),          # snowmen + melody
        (0x020F10, 12,    "town_misc",   "misc", {}),
        (0x020F1C, 960,   "town_items",  "bitfield", {"density": 0.1}),  # buried
        (0x0212DC, 8,     "town_misc",   "misc", {}),
        (0x0212E4, 56,    "town_misc",   "misc", {}),          # mother_mail
        (0x02131C, 50,    "town_misc",   "misc", {}),
        (0x02134E, 32,    "town_misc",   "bitfield", {"density": 0.5}),  # npc_used
        (0x02136E, 58,    "town_misc",   "misc", {}),
        (0x0213A8, 63,    "museum",      "bitfield", {"density": 0.9}),
        (0x0213E7, 9,     "town_misc",   "zero", {}),
        (0x0213F0, 16,    "town_misc",   "misc", {}),
    ]
    + [(0x021400 + i * 544, None, None, "sub", {"sub": DESIGN}) for i in range(8)]  # Able Sisters
    + [
        (0x022500, 64,    "town_misc",   "misc", {}),   # pad + time_delta + pad
        (0x022540, None,  None,          "sub", {"sub": ISLAND}),
        (0x023E40, 40,    "town_misc",   "misc", {}),
        (0x023E68, 160,   "town_misc",   "misc", {}),          # fish records
        (0x023F08, 24,    "town_misc",   "zero", {}),
        (0x023F20, 576,   "town_misc",   "misc", {}),          # mask_cat
        (0x024160, 12,    "town_misc",   "misc", {}),
        (0x02416C, 8,     "town_misc",   "misc", {}),
        (0x024174, 4,     "town_misc",   "misc", {}),
        (0x024178, 12,    "town_misc",   "misc", {}),
        (0x024184, 28,    "town_misc",   "misc", {}),
        (0x0241A0, 8,     "town_misc",   "misc", {}),
        (0x0241A8, 248,   "town_misc",   "zero", {}),
    ]
)

# --- mCD_others_c payload, include/m_card.h:146-185 ---
# The MemcardHeader_c (comment/banner/icon, 5184 B) and the 32 B pad are GC
# memory-card furniture; the DC replaces them with a VMS header, so the payload
# model starts at the first keep-block.
KEEP_ORIGINAL = (
    [(0x000, 128, "keep_original", "misc", {})]
    + [(0x080 + i * 544, None, None, "sub", {"sub": DESIGN}) for i in range(96)]
    + [(0xCC80, 32, "keep_original", "zero", {})]
)

KEEP_MAIL = (
    [(0x000, 100, "keep_mail", "misc", {})]
    + [(0x064 + i * 298, None, None, "sub", {"sub": MAIL}) for i in range(160)]
)

KEEP_DIARY = (
    [(0x000, 2, "keep_diary", "misc", {})]
    + [(0x002 + i * 992, 992, "keep_diary", "text", {"kind": "diary"}) for i in range(48)]
)

# absolute offsets inside mCD_others_c; we only model from 0x1460 onward
OTHERS_PAYLOAD = [
    (0x00000, None, None, "sub", {"sub": KEEP_ORIGINAL}),   # 52384
    (0x0CCA0, None, None, "sub", {"sub": KEEP_MAIL}),       # 47780
    (0x184A4, None, None, "sub", {"sub": KEEP_DIARY}),      # 47618
]
OTHERS_PAYLOAD_SIZE = 52384 + 47780 + 47618          # 147782
OTHERS_PAYLOAD_TAIL = 153056 - 5184 - 32 - OTHERS_PAYLOAD_SIZE   # inter-block align pad

# The DC-relevant unique payload: one Save_t + the three keep-blocks.
UNIQUE_PAYLOAD = SIZEOF["Save_t"] + OTHERS_PAYLOAD_SIZE       # 295910

# ---------------------------------------------------------------------------
# VMU budget constants (verified: see kb/save-budget.md "Sources")
# ---------------------------------------------------------------------------
VMU_BLOCK = 512
VMU_USER_BLOCKS = 200                       # 256 physical - FAT/dir/root
VMU_USER_BYTES = VMU_BLOCK * VMU_USER_BLOCKS      # 102400
VMS_HEADER_NO_ICON = 128                    # $00..$80 incl. 32 B icon palette
VMS_ICON_FRAME = 512
VMS_EYECATCH = {0: 0, 1: 8064, 2: 4544, 3: 2048}


def vms_header_bytes(icons=1, eyecatch=0):
    return VMS_HEADER_NO_ICON + VMS_ICON_FRAME * icons + VMS_EYECATCH[eyecatch]


def blocks_for(payload_bytes, icons=1, eyecatch=0):
    total = vms_header_bytes(icons, eyecatch) + payload_bytes
    return (total + VMU_BLOCK - 1) // VMU_BLOCK


def flatten(regions, base=0, out=None):
    """Expand a region tree into a flat [(abs_off, size, group, kind, params)]."""
    if out is None:
        out = []
    for off, size, group, kind, params in regions:
        if kind == "sub":
            flatten(params["sub"], base + off, out)
        else:
            out.append((base + off, size, group, kind, params))
    return out


def validate(regions, total_size, name):
    """Assert the region list tiles [0, total_size) exactly."""
    flat = sorted(flatten(regions), key=lambda r: r[0])
    pos = 0
    for off, size, group, kind, _ in flat:
        if off != pos:
            raise AssertionError(
                f"{name}: gap/overlap at 0x{pos:X}: next region '{group}' "
                f"starts at 0x{off:X}")
        pos += size
    if pos != total_size:
        raise AssertionError(f"{name}: tiled {pos} bytes, expected {total_size}")
    return flat


if __name__ == "__main__":
    st = validate(SAVE_T, SIZEOF["Save_t"], "Save_t")
    ko = validate(KEEP_ORIGINAL, SIZEOF["mCD_keep_original_c"], "keep_original")
    km = validate(KEEP_MAIL, SIZEOF["mCD_keep_mail_c"], "keep_mail")
    kd = validate(KEEP_DIARY, SIZEOF["mCD_keep_diary_c"], "keep_diary")
    print(f"Save_t         : {len(st):5d} regions, {SIZEOF['Save_t']} bytes  OK")
    print(f"keep_original  : {len(ko):5d} regions, {SIZEOF['mCD_keep_original_c']} bytes  OK")
    print(f"keep_mail      : {len(km):5d} regions, {SIZEOF['mCD_keep_mail_c']} bytes  OK")
    print(f"keep_diary     : {len(kd):5d} regions, {SIZEOF['mCD_keep_diary_c']} bytes  OK")
    print(f"unique payload : {UNIQUE_PAYLOAD} bytes")
    print(f"VMU user bytes : {VMU_USER_BYTES}  (needs {UNIQUE_PAYLOAD / VMU_USER_BYTES:.2f}:1)")
