#!/usr/bin/env python3
"""icache_map.py — how a hot symbol set lands in the SH-4's instruction cache.

WHY. The SH7750's instruction cache is 8 KB, DIRECT-MAPPED, 32-byte lines:
256 lines, and line index = bits [12:5] of the address. Two functions whose
addresses are congruent mod 8192 evict each other every time control passes
between them, no matter how much cache is free elsewhere. Our `.text` is
~2.88 MB and Flycast models no instruction cache at all, so nothing the
emulator has ever reported can see this (kb/RESUME.md §0h).

This is the HOST-SIDE half of that investigation, and it is free: no burn, no
emulator, seconds to run. It cannot measure stalls — only dc/src/dc_pmcr.c on
real hardware can do that — but it can do two things that matter before a burn
is spent:

  1. SIZE THE HOT SET. If the per-frame hot working set exceeds 8 KB, conflict
     misses are arithmetically guaranteed and the only question left is how
     expensive they are. If it fits, the aliasing hypothesis is dead and the
     hardware gap is somewhere else.
  2. NAME THE COLLIDING PAIRS. Line-index occupancy says which hot functions
     are fighting over the same 32 bytes of cache — which is exactly the input
     an ld section-ordering file needs.
  3. EMIT THAT ORDERING FILE (`--emit-order`). See "F5 — THE ORDERING FILE"
     below; the tracked output is `dc/section-order.txt`.

⚠️ WHAT IT DOES NOT KNOW. It has no call profile: it treats every symbol
matched by --hot as equally hot and assumes each is entered once per frame.
A collision between two functions that never run in the same frame costs
nothing. Read the output as "candidates to order", never as "misses".

USAGE
    sh-elf-nm -S --defined-only build/AnimalCrossing.elf > nm.txt
    python3 tools/dcopt/icache_map.py nm.txt
    python3 tools/dcopt/icache_map.py nm.txt --hot '^_dl_G_' --hot '^_dc_gx_'
    python3 tools/dcopt/icache_map.py nm.txt --top 40 --size 8192 --line 32
    python3 tools/dcopt/icache_map.py nm.txt --emit-order dc/section-order.txt

F5 — THE ORDERING FILE
======================
Everything below was verified against GNU ld 2.45.1 (`sh-elf-ld`) in the
opencrossing-dc:sdk image on 2026-08-09. None of it is inferred.

*** The flag is `--section-ordering-file FILE`, not `--symbol-ordering-file`.
    The latter is an LLD flag and does not exist in GNU ld.

*** THE FILE IS A LINKER-SCRIPT `SECTIONS` FRAGMENT, NOT A LIST OF NAMES.
    "one section name per line" is a SYNTAX ERROR:
        sh-elf-ld:order.txt:2: syntax error in section-ordering-file script
    The real form (ld.info, "--section-ordering-file") is
        .text : { *(.text.first) ; *(.text.z*) }
    and it is *prepended* to the matching output section's existing statement
    list. So the output section named must ALREADY EXIST in the linker script
    in use — naming one that does not is a hard error:
        sh-elf-ld:bad.txt:1: error: output section '.nosuchout' must already exist
    KOS's `shlelf.xc` defines `.text` (`*(.text .stub .text.* ...)`), which is
    the one we target.

*** SECTIONS NOT NAMED KEEP THEIR DEFAULT ORDER, AFTER THE NAMED ONES.
    Verified on a 5-function toy link: baseline fa fb fc fd fe; with
    `.text : { *(.text.fd) ; *(.text.fb) }` the result is fd fb fa fc fe —
    the two named sections move to the front, the remaining three keep their
    original relative order behind them. The whole design rests on this.

*** A NAMED SECTION THAT DOES NOT EXIST IS SILENTLY IGNORED — no warning, no
    error. So a stale ordering file degrades to a no-op for the stale entries
    rather than breaking the link. (It also means a typo is invisible: a
    wrong section name is exactly as quiet as a right one. This is why the
    name derivation below was checked against a real object file rather than
    assumed.)

*** `#` COMMENTS ARE ACCEPTED, whole-line and trailing. Verified by linking a
    file whose first two lines were `# hash comment` / `# another`: the link
    succeeded AND the ordering still took effect. `/* ... */` works too. We
    emit `#`.

*** ⚠️ THE STARTUP MUST STAY AT 0x8c010000. `_kos_startup.o`'s plain `.text`
    holds `start`, the ELF entry, and the Dreamcast loader enters 1ST_READ.BIN
    at its first byte. A fragment that begins with `*(.text.something)` hoists
    that something in front of `start` and the image does not boot — verified
    on the toy (`_start_stub` landed at +0xc instead of 0). Every file this
    tool emits therefore opens with a bare `*(.text)`, which re-anchors all
    plain-`.text` inputs (startup first, since it is the STARTUP file) ahead of
    the hot block. DO NOT HAND-EDIT THAT LINE OUT.

*** SECTION NAME vs SYMBOL NAME — the trap. sh-elf sets USER_LABEL_PREFIX="_",
    so `nm` prints `_dc_gx_backend_submit`, but the section GCC emits under
    `-ffunction-sections` is `.text.dc_gx_backend_submit` — WITHOUT the
    prefix. Evidence, same object file, same build:
        $ sh-elf-objdump -h dc/build/obj/dc/src/dc_pvr.c.o | grep submit
         12 .text.dc_gx_backend_submit 00002788 ...
        $ sh-elf-nm --defined-only dc/build/obj/dc/src/dc_pvr.c.o | grep submit
        00000000 T _dc_gx_backend_submit
    and in `dc/build/AnimalCrossing.map`:
         .text.dc_gx_backend_submit
                        0x8c221118     0x2788 .../dc_pvr.c.o
    Hence `sym_to_section()` strips exactly ONE leading underscore. C++ names
    follow the same rule: `__ZN5emu649dl_G_TRINEv` -> `.text._ZN5emu649dl_G_TRINEv`.

*** ⚠️ FLYCAST MODELS NO INSTRUCTION CACHE. Nothing here can be A/B'd in the
    emulator. `dc/src/dc_pmcr.c`'s `istall` event on a real burned CD-R is the
    only instrument (kb/RESUME.md §0h, kb/research-fps-ideas.md §F5).

No third-party modules.
"""

import argparse
import datetime
import os
import re
import sys
from collections import defaultdict

# The default hot set: the town frame as this project has measured it.
#   - emu64's dispatch loop and the two opcodes that are 73.7 % of the draw
#     (kb/research-sh4zam-gap.md §3)
#   - our own GX state machine and the PVR backend it feeds
#   - G3's cull, which now runs at TRIN entry on every batch
#   - the audio pump, which costs ~2.3 ms + ~265 us/voice every tick
# sh-elf prefixes C symbols with '_' (kb/RESUME.md session 4 item 10), so every
# pattern here starts with it.
#
# ⚠️ CORRECTED 2026-08-09. The original list had FOUR patterns that matched
# nothing at all: `^_dl_G_`, `^_emu64` (beyond `_emu64_taskstart`), `^_cu_trin`
# and anything for the cull's statics. emu64 is C++, so every opcode handler is
# a MANGLED name — `__ZN5emu649dl_G_TRINEv`, not `_dl_G_TRIN` — and the cull's
# trampolines are file-static C++ (`__ZL7cu_trinPv`). The interpreter that G1
# measured as most of the draw was therefore ABSENT from the hot set, and any
# pressure figure computed before this date understates it.
DEFAULT_HOT = [
    r"^_emu64_taskstart",
    r"^__ZN5emu64",          # every emu64 method: dispatch, dl_G_*, set_position
    r"^_dl_G_",              # kept in case a handler is ever exported as C
    r"^_dc_gx_",
    r"^_GX[A-Z]",
    r"^_dc_pvr_",
    r"^__ZL10cull_batchP5emu64",
    r"^__ZL7cu_trinPv",
    r"^__ZL13cu_trin_indepPv",
    r"^_dc_emu64_cull",
    r"^_emit_projected$",    # dc_pvr.c statics: the per-vertex emit path
    r"^_emit_triangle_raw$",
    r"^_fog_program$",
    r"^_PSMTX",
    r"^_C_MTX",
    r"^_Nas_",
    r"^_pc_audio_",
]

# ---------------------------------------------------------------------------
# THE ORDERING POLICY — "call adjacency proxy"
# ---------------------------------------------------------------------------
# We have no call profile (see the docstring), so the ordering is a hand-argued
# proxy for one: put the functions that call each other PER VERTEX next to each
# other, in the order control passes between them, so that the innermost draw
# loop occupies one contiguous run of cache lines instead of being scattered
# across ~2.5 MB where any two of them can be congruent mod 8192.
#
# TIER 1 is that innermost loop, in call order:
#     emu64 dispatch -> the TRI/TRIN/QUAD/VTX opcode handlers -> G3's cull at
#     TRIN entry -> emu64::set_position -> the GX* per-vertex setters ->
#     dc_gx's vertex commit/flush -> dc_gx_backend_submit and the dc_pvr
#     vertex loop it runs (emit_projected / emit_triangle_raw / fog_program).
# TIER 2 is the rest of whatever --hot matched, grouped by the pattern that
#     matched it so that a TU's functions stay together, in pattern order.
# TIER 3 is everything else: NOT NAMED AT ALL. ld leaves unnamed sections in
#     their default order behind the named ones (verified — see the docstring),
#     which is exactly the "order what matters, ignore the rest" property the
#     ld.info page advertises.
#
# Each entry is (regex, why). Order in this list IS the emitted order; within
# one entry, symbols are emitted in address order so the file is deterministic.
# A regex that matches nothing is fine — it is reported, not fatal.
INNER_LOOP = [
    (r"^__ZN5emu6417emu64_taskstart_rEP3Gfx$", "emu64 dispatch loop"),
    (r"^_emu64_taskstart$",                    "its C entry"),
    (r"^__ZN5emu6418dl_G_TRIN_INDEPENDEv$",    "TRIN_INDEPEND: 73.7% of the draw"),
    (r"^__ZN5emu649dl_G_TRINEv$",              "the TRIN body"),
    (r"^__ZN5emu649dl_G_TRI1Ev$",              "TRI1"),
    (r"^__ZN5emu649dl_G_TRI2Ev$",              "TRI2"),
    (r"^__ZN5emu649dl_G_QUADEv$",              "QUAD"),
    (r"^__ZN5emu6410dl_G_QUADNEv$",            "QUADN"),
    (r"^__ZN5emu648dl_G_VTXEv$",               "VTX: the other big opcode"),
    (r"^__ZL13cu_trin_indepPv$",               "G3's TRIN-entry cull trampoline"),
    (r"^__ZL7cu_trinPv$",                      "G3's TRIN cull trampoline"),
    (r"^__ZL10cull_batchP5emu64$",             "G3's AABB cull proper"),
    (r"^__ZN5emu6412set_positionEj$",          "per-vertex transform entry"),
    (r"^_GXBegin$",                            "batch open"),
    (r"^_GXPosition3f32$",                     "per-vertex setters, hottest first"),
    (r"^_GXPosition2f32$", ""),
    (r"^_GXPosition2u16$", ""),
    (r"^_GXNormal3f32$", ""),
    (r"^_GXColor4u8$", ""),
    (r"^_GXColor1u32$", ""),
    (r"^_GXTexCoord2f32$", ""),
    (r"^_GXTexCoord2s16$", ""),
    (r"^_GXTexCoord2u8$", ""),
    (r"^_dc_gx_commit_vertex$",                "dc_gx's per-vertex commit"),
    (r"^_dc_gx_flush_if_begin_complete$", ""),
    (r"^_dc_gx_flush_vertices$", ""),
    (r"^_dc_gx_mark_dirty$", ""),
    (r"^_dc_gx_vtxid_arm$",                    "G-B's vertex-index side channel"),
    (r"^_dc_gx_vtxid_disarm$", ""),
    (r"^_GXEnd$",                              "batch close"),
    (r"^_dc_gx_aabb_is_offscreen$",            "the late cull GXEnd still runs"),
    (r"^_dc_gx_backend_submit$",               "the PVR backend's vertex loop"),
    (r"^_emit_projected$", ""),
    (r"^_emit_triangle_raw$", ""),
    (r"^_fog_program$", ""),
]


def sym_to_section(name, prefix=".text."):
    """Map an nm symbol name to the -ffunction-sections section name.

    sh-elf sets USER_LABEL_PREFIX="_", so the ASSEMBLER symbol carries a
    leading underscore that the SECTION name does not. Verified against a real
    object file — see the "SECTION NAME vs SYMBOL NAME" block in the module
    docstring; getting this backwards makes the whole flag a silent no-op,
    because ld ignores a section name that matches nothing without a word.
    """
    return prefix + (name[1:] if name.startswith("_") else name)


def parse_nm(path):
    """Yield (addr, size, kind, name) for sized text symbols.

    `nm -S` prints `addr size type name`; symbols with no size print three
    fields. Only 't'/'T' are code — a data symbol shares no cache with them
    (the SH-4's caches are split), and counting one would inflate the hot set
    with something that cannot cause an instruction-cache conflict.
    """
    out = []
    with open(path) as fh:
        for line in fh:
            f = line.split()
            if len(f) != 4:
                continue
            addr, size, kind, name = f
            if kind not in ("t", "T"):
                continue
            try:
                out.append((int(addr, 16), int(size, 16), kind, name))
            except ValueError:
                continue
    return out


def emit_order(path, hot, pats, a, hot_bytes, text_bytes):
    """Write a GNU ld --section-ordering-file for the hot set.

    The emitted file is a `SECTIONS` fragment, not a list of names — see the
    "F5 — THE ORDERING FILE" block in the module docstring, every clause of
    which was verified against sh-elf-ld 2.45.1 rather than assumed.

    Returns (n_tier1, n_tier2, n_unnamed_hot) for the caller to report.
    """
    by_name = {}
    for addr, size, _, name in hot:
        # A static symbol can appear once per TU under the same name; the
        # section name is the same for all of them and `*(.text.foo)` matches
        # every one. Keep the lowest address so the file is deterministic.
        if name not in by_name or addr < by_name[name][0]:
            by_name[name] = (addr, size)

    remaining = dict(by_name)
    lines = []
    tier1_syms = []

    lines.append("  # ---- tier 0: the startup MUST stay first ----")
    lines.append("  #")
    lines.append("  # _kos_startup.o's plain .text holds `start`, the ELF entry, and the")
    lines.append("  # Dreamcast loader enters 1ST_READ.BIN at its first byte. Anything")
    lines.append("  # hoisted in front of it does not boot. Do not delete this line.")
    lines.append("  *(.text) ;")
    lines.append("")
    lines.append("  # ---- tier 1: the innermost draw loop, in call order ----")
    for pat, why in INNER_LOOP:
        rx = re.compile(pat)
        hits = sorted((v[0], k) for k, v in remaining.items() if rx.search(k))
        if not hits:
            lines.append("  # (no match: %s)" % pat)
            continue
        for _, name in hits:
            del remaining[name]
            tier1_syms.append(name)
            note = ("  # %s" % why) if why else ""
            lines.append("  *(%s) ;%s" % (sym_to_section(name), note))
    lines.append("")

    lines.append("  # ---- tier 2: the rest of the hot set, grouped by --hot pattern ----")
    n_tier2 = 0
    for p in pats:
        hits = sorted((v[0], k) for k, v in remaining.items() if p.search(k))
        if not hits:
            continue
        lines.append("  # %s" % p.pattern)
        for _, name in hits:
            del remaining[name]
            n_tier2 += 1
            lines.append("  *(%s) ;" % sym_to_section(name))
    lines.append("")
    lines.append("  # ---- tier 3: everything else is DELIBERATELY NOT NAMED ----")
    lines.append("  # ld leaves unnamed input sections in their default order behind the")
    lines.append("  # named ones, so the rest of .text is untouched. Verified on a toy")
    lines.append("  # link with sh-elf-ld 2.45.1; the whole design rests on it.")

    t1_bytes = sum(by_name[n][1] for n in tier1_syms)
    stamp = datetime.datetime.utcnow().strftime("%Y-%m-%d")

    head = [
        "# dc/section-order.txt — GNU ld --section-ordering-file (F5, i-cache packing)",
        "#",
        "# GENERATED — do not hand-edit. Regenerate with:",
        "#   docker run --rm --platform linux/arm64 -v $PWD:/work opencrossing-dc:sdk \\",
        "#       bash -c 'sh-elf-nm -S --defined-only /work/dc/build/AnimalCrossing.elf' > /tmp/nm.txt",
        "#   python3 tools/dcopt/icache_map.py /tmp/nm.txt --emit-order dc/section-order.txt",
        "#",
        "# ld ignores '#' comments in an ordering file (verified, sh-elf-ld 2.45.1),",
        "# and silently ignores a section name that matches nothing — so a stale entry",
        "# here degrades to a no-op rather than breaking the link.",
        "#",
        "# PROVENANCE",
        "#   generated      : %s (UTC) by tools/dcopt/icache_map.py" % stamp,
        "#   nm input       : %s" % os.path.abspath(a.nm),
        "#   elf            : %s" % (a.order_elf or "(not stated — pass --order-elf)"),
        "#   --hot patterns : %s" % " ".join(p.pattern for p in pats),
        "#",
        "# THE NUMBER THIS RESTS ON",
        "#   icache             : %d B, direct-mapped, %d B lines"
        % (a.size, a.line),
        "#   sized .text        : %d B (%.2f MB)" % (text_bytes,
                                                     text_bytes / 1048576.0),
        "#   hot set            : %d symbols, %d B (%.1f KB) = %.2fx the %d B cache"
        % (len(by_name), hot_bytes, hot_bytes / 1024.0,
           hot_bytes / float(a.size), a.size),
        "#   tier 1 (inner loop): %d symbols, %d B = %.2fx the %d B cache"
        % (len(tier1_syms), t1_bytes, t1_bytes / float(a.size), a.size),
        "#",
        "#   ⚠️ The hot set does not fit, so this file cannot remove capacity misses.",
        "#   It removes CONFLICT misses inside the inner loop by making it contiguous.",
        "#   ⚠️ Flycast models no instruction cache: this is UNMEASURABLE in the",
        "#   emulator. dc/src/dc_pmcr.c's istall event on a real burn is the only",
        "#   instrument (kb/research-fps-ideas.md §F5).",
    ]
    if a.order_note:
        head.append("#")
        for ln in a.order_note.splitlines():
            head.append("# NOTE: %s" % ln)
    head.append("")
    head.append("%s : {" % a.order_section)

    with open(path, "w") as fh:
        fh.write("\n".join(head + lines) + "\n}\n")

    return len(tier1_syms), n_tier2, len(remaining)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("nm", help="output of sh-elf-nm -S --defined-only")
    ap.add_argument("--hot", action="append", default=None,
                    help="regex for a hot symbol (repeatable). Replaces the "
                         "built-in set.")
    ap.add_argument("--size", type=int, default=8192, help="icache bytes")
    ap.add_argument("--line", type=int, default=32, help="line bytes")
    ap.add_argument("--top", type=int, default=20,
                    help="how many worst-contended lines to print")
    ap.add_argument("--emit-order", metavar="FILE", default=None,
                    help="write a GNU ld --section-ordering-file for the hot "
                         "set (F5). The tracked one is dc/section-order.txt.")
    ap.add_argument("--order-section", default=".text", metavar="SEC",
                    help="output section the fragment targets. It must already "
                         "exist in the linker script in use, or ld errors. "
                         "(default: .text, which KOS's shlelf.xc defines)")
    ap.add_argument("--order-elf", default=None, metavar="PATH",
                    help="record which ELF the nm came from, for provenance")
    ap.add_argument("--order-note", default=None, metavar="TEXT",
                    help="free-text note recorded in the generated file's "
                         "header. The hot-bytes-vs-cache numbers are always "
                         "recorded whether or not this is given.")
    a = ap.parse_args()

    nlines = a.size // a.line
    pats = [re.compile(p) for p in (a.hot if a.hot else DEFAULT_HOT)]

    syms = parse_nm(a.nm)
    if not syms:
        sys.exit("no sized text symbols in %s — was it `nm -S`?" % a.nm)

    text_bytes = sum(s for _, s, _, _ in syms)
    hot = [t for t in syms if any(p.search(t[3]) for p in pats)]
    hot_bytes = sum(s for _, s, _, _ in hot)

    print("== .text ==")
    print("  sized text symbols : %d" % len(syms))
    print("  sized text bytes   : %d (%.2f MB)" % (text_bytes,
                                                   text_bytes / 1048576.0))
    print("  icache             : %d B, direct-mapped, %d B lines, %d lines"
          % (a.size, a.line, nlines))
    print()
    print("== the hot set ==")
    print("  patterns           : %s" % " ".join(p.pattern for p in pats))
    print("  matched symbols    : %d" % len(hot))
    print("  hot bytes          : %d (%.1f KB)" % (hot_bytes,
                                                   hot_bytes / 1024.0))
    if a.size:
        print("  PRESSURE           : %.1fx the whole instruction cache"
              % (hot_bytes / float(a.size)))
    print()

    if hot_bytes <= a.size:
        print("  ⇒ the hot set FITS in the icache. Conflict misses are then a")
        print("    question of layout only, not of capacity.")
    else:
        print("  ⇒ the hot set does NOT fit. Capacity misses are guaranteed")
        print("    before any question of aliasing is asked: the set must")
        print("    shrink (less inlining, -Os on hot TUs) or be ordered so")
        print("    that what runs together lives together.")
    print()

    # Line-index occupancy. A symbol occupies every line its byte range covers,
    # modulo the cache size — which is what makes this direct-mapped rather
    # than a set count.
    occ = defaultdict(list)
    for addr, size, _, name in hot:
        first = addr // a.line
        last = (addr + max(size, 1) - 1) // a.line
        span = last - first + 1
        if span >= nlines:
            # A single symbol larger than the whole cache: it evicts itself.
            for i in range(nlines):
                occ[i].append(name)
            continue
        for i in range(span):
            occ[(first + i) % nlines].append(name)

    counts = [(len(occ[i]), i) for i in range(nlines)]
    counts.sort(reverse=True)
    empty = sum(1 for c, _ in counts if c == 0)

    print("== icache line occupancy over the hot set ==")
    print("  lines with no hot code : %d of %d" % (empty, nlines))
    print("  worst line             : %d distinct hot symbols" % counts[0][0])
    print()
    print("  the %d most contended lines (line: symbols sharing it):" % a.top)
    for c, i in counts[:a.top]:
        if c == 0:
            break
        names = sorted(set(occ[i]))
        shown = ", ".join(names[:6])
        more = "" if len(names) <= 6 else " (+%d more)" % (len(names) - 6)
        print("    %3d  x%-3d  %s%s" % (i, c, shown, more))

    print()
    print("⚠️ These are CANDIDATES TO ORDER, not measured misses. This tool has")
    print("   no call profile and no idea which of these run in the same")
    print("   frame. Only dc/src/dc_pmcr.c on real hardware prices a stall.")

    if a.emit_order:
        n1, n2, n3 = emit_order(a.emit_order, hot, pats, a, hot_bytes,
                                text_bytes)
        print()
        print("== wrote %s ==" % a.emit_order)
        print("  tier 1 (inner loop)  : %d sections" % n1)
        print("  tier 2 (rest of hot) : %d sections" % n2)
        print("  tier 3 (unnamed)     : everything else, default order")
        if n3:
            print("  ⚠️ %d hot symbols matched no tier — this should be 0" % n3)
        print("  ⚠️ UNMEASURABLE IN FLYCAST. It models no instruction cache.")
        print("     dc_pmcr.c's istall on a burned CD-R is the only instrument.")


if __name__ == "__main__":
    main()
