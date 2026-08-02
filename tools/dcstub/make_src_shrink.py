#!/usr/bin/env python3
"""
make_src_shrink.py — build the DC_SRC_SHRINK source tree (kb/STATE.md, step S3).

WHY THIS EXISTS
---------------
The image is ~8.8 MB over the 16 MB inequality and `.bss` is where the budget
has to come from.  A handful of numeric literals in the vendored decomp size
buffers that this port either never writes (dead code paths) or over-allocates
by a wide margin.  Rewriting those literals recovers 1,175,776 B with no
codegen change of any kind.

WHY A REWRITER AND NOT AN EDIT
------------------------------
CLAUDE.md §1: `src/` and `include/` are vendored decomp and are never edited.
Nothing here touches them.  Every rewrite is emitted into a scratch tree under
`dc/build/shrinksrc/`, mirroring repo-relative paths, and `dc/Makefile` swaps
it in — either per-TU (a `.c` file listed in `shrink.list`) or by putting the
scratch tree at the FRONT of the include path (a `.h` / `.c_inc` shadow).
`DC_SRC_SHRINK=0` reverts to a byte-identical build.

This mirrors `make_stub_data.py` / `DC_ASSET_STUB` exactly; read that pair
first if this file is unfamiliar.

THE FAILURE MODE THIS FILE IS BUILT AROUND
------------------------------------------
A rewriter whose regex silently stops matching produces a build that looks fine
and saves nothing — or, worse, half-applies and corrupts memory.  So:

  * every rule is ANCHORED and declares how many times it must match;
  * a count mismatch is a hard error, never a warning;
  * where a shrink could in principle be seen by one TU and not another, the
    rewrite is confined to a single TU and carries a compile-time assert on the
    *unshrunk* type it is twinned against.

WHAT IT REWRITES  (byte savings measured against the real ELF's symbol sizes)
----------------------------------------------------------------------------
S1  actor overlay arenas                                        -422,192
    aSTR_overlay 294,912->512 · aINS_overlay 21,528->72 ·
    aNPC_{n,s,k,e}_overlay 75,840->192 · aGYO_overlay 30,720->32
    These are DEAD, not "mutually exclusive buffers to union": across the whole
    tree the arenas are only ever ADDRESS-taken.  `ACTOR_DLFTBL::alloc_buf`,
    the one thing that ever points at them, is assigned, compared and NULLed
    and never dereferenced (verified: every occurrence in `src/`,
    `include/m_actor_dlftbls.h:16` says so, and the three writers of a non-NULL
    `alloc_buf` are only ever stored into function-pointer slots that nothing
    calls).  The ELEMENT shrinks, never the count, so every loop bound, index,
    pointer-identity compare and NULL check is byte-identical.

S2  prbuf 614,400->32                                           -614,368
    `src/game/m_play.c`.  Its only writer is `copy_efb_to_texture()`, whose
    payload is `GXCopyTex()` — a loud no-op on DC (`dc/src/dc_gx.c:1524`)
    recorded into a display list that `GXBeginDisplayList`/`GXEndDisplayList`
    never actually record (`:1613`, returns 0 bytes).  prbuf therefore only
    ever contains zeroes.  Its only reader is the prerender blit, which is
    neutralised here to three `gDPNoOp`s — leaving it in place would make
    emu64 sample 614,400 B out of a 32 B array.

S4  audiomemory 0x90000->0x76000                                -106,496
    `src/static/jaudio_NES/game/game64.c_inc:587`, handed whole to
    `Jac_HeapSetup()` and consumed only through `OSAlloc2()`.  Every consumer,
    enumerated: dac 3x2240 + dspbuf 3x2240 + cpubuf 3x2240 + tmp_buf 2240 +
    acmdBuf 0x70000 = 481,152 B.  0x76000 = 483,328 leaves 2,176 B of margin.
    DO NOT GO FURTHER: `Nas_WaveDmaNew` (`internal/system.c:431`) silently
    `break`s on heap exhaustion and drops voices with no error.

S5  sys_stacks 16,384->48                                        -16,336
    `include/sys_stacks.h`.  `osCreateThread2` on DC (`dc/src/dc_os.c:250`)
    ignores its stack argument entirely and `osStartThread` starts nothing, so
    no byte of these three arrays is ever used as a stack.  Shrinking the
    MACROS rather than the arrays keeps `src/main.c:55`'s
    `memset(padmgrStack, 0xEB, PADMGR_STACK_SIZE)` and the three
    `stack + SIZE` expressions consistent with the new sizes.

NOT DONE — the fifth candidate was checked and REFUTED
------------------------------------------------------
`CALLSTACK[0x8000]` and `pc_task_buf[2][1600]` are NOT dead scaffolding.
`CALLSTACK` is a live 128-slot x 256 B pool of task frames
(`dvdthread.c:45 GetCallStack()`), written by `DVDT_AddTask`/`AddTaskHigh` for
real ARAM loads and drained by `pc_dvd_process_all_tasks()`
(`neosthread.c:86`).  `pc_task_buf` is the live audio command buffer:
`CreateAudioTask(pc_task_buf[cur], ...)` fills it and `RspStart2` consumes it
every frame (`neosthread.c:35-39`).  Shrinking either corrupts audio silently.

USAGE
-----
    python3 tools/dcstub/make_src_shrink.py [--out DIR] [--dry-run] [--quiet]

Default --out is dc/build/shrinksrc.  Files are written only when their content
actually changes, so re-running does not invalidate objects.
"""

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_OUT = REPO / "dc" / "build" / "shrinksrc"

# A compile-time assert that works in gnu89, gnu11 and C++ alike. `_Static_assert`
# is C11-only in principle and several of these TUs are -std=gnu89 or -x c++;
# the negative-array-size idiom has no dialect dependency.
def ASSERT(tag, expr):
    return ("typedef char dc_src_shrink_assert_%s[(%s) ? 1 : -1]; "
            "/* DC_SRC_SHRINK guard */" % (tag, expr))


# ---------------------------------------------------------------------------
# The rules.
# ---------------------------------------------------------------------------
# Each entry:  (repo-relative path, kind, [ (must_match, pattern, replacement) ])
#
# kind == "swap"   -> a .c TU; dc/Makefile compiles the copy instead of the
#                     original (the `shrinkify` function, mirroring `stubify`).
# kind == "shadow" -> a .h / .c_inc pulled in by #include; dc/Makefile puts
#                     $(SHRINKDIR)/include and $(SHRINKDIR)/src at the FRONT of
#                     INCLUDES so the copy wins.  VERIFIED with `sh-elf-gcc -H`
#                     for every consumer listed below; see the module docstring
#                     of dc/Makefile's DC_SRC_SHRINK block.
#
# `replacement` is passed to re.sub with count=1 per match, so backreferences
# work; it is applied with re.MULTILINE.

MARK = "/* DC_SRC_SHRINK */"

RULES = [
    # -- S1a: aSTR_overlay ---------------------------------------------------
    # SWAP, not a shadow of include/ac_structure.h: that header is #included
    # from sibling headers inside include/ (ac_shop.h, ac_kago.h, ...), and a
    # quoted include is resolved against the INCLUDING FILE'S directory first,
    # so -I can never shadow it there.  MEASURED with -H: a shadow reaches
    # ac_structure.c but not ac_npc.c — exactly the half-applied split this
    # tool must not ship.  The array is `static` and aSTR_OVERLAY_SIZE has no
    # other use in the tree, so rewriting the declaration in place is total.
    ("src/actor/ac_structure.c", "swap", [
        (1,
         r"^static u8 aSTR_overlay\[aSTR_ACTOR_TBL_COUNT\]\[aSTR_OVERLAY_SIZE\];$",
         "static u8 aSTR_overlay[aSTR_ACTOR_TBL_COUNT][0x10]; " + MARK),
    ]),

    # -- S1b: aINS_overlay ---------------------------------------------------
    # Confined to this one TU for the same reason: aINS_overlay_entry_c lives
    # in include/ac_insect_h.h, which is reached through sibling headers.
    # Rather than risk two TUs disagreeing on sizeof(), declare a local twin
    # with the same trailing fields and a 16 B payload, and assert that the
    # real type still has the layout the twin was derived from.
    #
    # `data[0x1C00]` is never read or written anywhere: aINS_actor_ct touches
    # only ->_1C00 and ->_1C04 (ac_insect.c:73-74), and nothing else in the
    # tree names aINS_overlay or aINS_overlay_entry_c.
    ("src/actor/ac_insect.c", "swap", [
        (1,
         r"^static aINS_overlay_c aINS_overlay;$",
         "/* " + "-" * 68 + "\n"
         " * DC_SRC_SHRINK: aINS_overlay_entry_c::data[0x1C00] is dead — only\n"
         " * _1C00/_1C04 are ever touched (see aINS_actor_ct below), and no\n"
         " * other TU names this object or its type.  Local twin with a 16 B\n"
         " * payload; the header type is left alone so no ODR split is even\n"
         " * possible.  The assert pins the layout the twin was derived from.\n"
         " * " + "-" * 68 + " */\n"
         + ASSERT("ac_insect_h", "sizeof(aINS_overlay_entry_c) == 0x1C08") + "\n"
         "typedef struct {\n"
         "    u8 data[0x10];\n"
         "    int _1C00;\n"
         "    int _1C04;\n"
         "} ATTRIBUTE_ALIGN(8) aINS_overlay_entry_dcshrink_c;\n"
         "typedef struct {\n"
         "    aINS_overlay_entry_dcshrink_c entries[3];\n"
         "} aINS_overlay_dcshrink_c;\n"
         "static aINS_overlay_dcshrink_c aINS_overlay;"),
        (1,
         r"^    aINS_overlay_entry_c\* overlay_p;$",
         "    aINS_overlay_entry_dcshrink_c* overlay_p; " + MARK),
    ]),

    # -- S1c: aNPC_{n,s,k,e}_overlay ----------------------------------------
    # SHADOW.  ac_npc_ctrl.c_inc is #included exactly once, from
    # src/actor/npc/ac_npc.c:130 as "../src/actor/npc/ac_npc_ctrl.c_inc".  That
    # relative path cannot resolve against the includer's own directory, so it
    # falls through to -I and lands in the shrink tree.  VERIFIED with -H:
    #   . /…/shrinksrc/include/../src/actor/npc/ac_npc_ctrl.c_inc
    # Both the struct definitions and the arrays live in this one file, so the
    # shadow is self-contained and cannot half-apply.
    #
    # aNPC_actor_class_overlay_c::buf[0x9D0] is deliberately NOT touched: it is
    # REAL storage — aNPC_keep_actor_class() hands its address out as a
    # NPC_ACTOR* (ac_npc_ctrl.c_inc:733) and aNPC_get_actor_area_proc returns
    # it for allocations up to 0x9D0 B.
    ("src/actor/npc/ac_npc_ctrl.c_inc", "shadow", [
        (1, r"^    u8 buf\[0x800\];$",  "    u8 buf[0x10]; " + MARK),
        (1, r"^    u8 buf\[0x2000\];$", "    u8 buf[0x10]; " + MARK),
        (1, r"^    u8 buf\[0x3000\];$", "    u8 buf[0x10]; " + MARK),
        (1, r"^    u8 buf\[0x2800\];$", "    u8 buf[0x10]; " + MARK),
    ]),

    # -- S1d: aGYO_overlay ---------------------------------------------------
    # SWAP: ac_gyoei.c is compiled directly, so -I cannot reach it.  The type
    # and the array are both local to this TU; the only consumer is
    # `ctrl->overlay_p = aGYO_overlay[i].buf` (address only, NULL-compared in
    # aGYO_actor_dt and never dereferenced).
    ("src/actor/ac_gyoei.c", "swap", [
        (1, r"^    u8 buf\[0x3C00\];$", "    u8 buf[0x10]; " + MARK),
    ]),

    # -- S2: prbuf -----------------------------------------------------------
    # SWAP.  Two halves, both required — shrinking the array without killing
    # the blit hands emu64 a 32 B buffer to sample 614,400 B out of.
    ("src/game/m_play.c", "swap", [
        (1,
         r"^static u8 prbuf\[\(2\*SCREEN_WIDTH\) \* \(2\*SCREEN_HEIGHT\) \* sizeof\(u16\)\] ATTRIBUTE_ALIGN\(32\);.*$",
         "/* DC_SRC_SHRINK: prbuf is write-only-by-a-no-op on Dreamcast.  Its one\n"
         " * writer is copy_efb_to_texture() below, whose payload GXCopyTex() is a\n"
         " * stub (dc/src/dc_gx.c:1524) recorded into a display list that\n"
         " * GXBeginDisplayList/GXEndDisplayList do not record (:1613, returns 0).\n"
         " * So the buffer only ever holds zeroes.  Its one reader — the prerender\n"
         " * blit ~700 lines down — is turned into gDPNoOps by the same pass.\n"
         " * 614,400 B -> 32 B; the address stays distinct, non-NULL and 32-aligned\n"
         " * so copy_efb_to_texture(&fb_disp, &prbuf) still type-checks and runs. */\n"
         "static u8 prbuf[32] ATTRIBUTE_ALIGN(32);"),
        (1,
         r"^        gDPSetTextureImage_Dolphin\(rect_disp\+\+, G_IM_FMT_I, G_IM_SIZ_16b, \(2\*SCREEN_HEIGHT\), \(2\*SCREEN_WIDTH\), prbuf\);$",
         "        /* DC_SRC_SHRINK: prbuf is 32 B and always zero — see its\n"
         "         * declaration.  The three commands that would sample it become\n"
         "         * no-ops; rect_disp advances by the same three words, so the\n"
         "         * display list keeps its shape and SET_POLY_OPA_DISP is right. */\n"
         "        gDPNoOp(rect_disp++);"),
        (1,
         r"^        gDPSetTile_Dolphin\(rect_disp\+\+, G_DOLPHIN_TLUT_DEFAULT_MODE, 0, 0, 0, 0, 0, 0\);$",
         "        gDPNoOp(rect_disp++); " + MARK),
        (1,
         r"^        gSPTextureRectangle\(rect_disp\+\+, 0, 0, \(\(2\*SCREEN_WIDTH\) - 1\) << 2, \(\(2\*SCREEN_HEIGHT\) - 1\) << 2, G_TX_RENDERTILE, 0, 0, \(256\) << G_TEXTURE_IMAGE_FRAC,\n"
         r"                            \(256\) << G_TEXTURE_IMAGE_FRAC\);$",
         "        gDPNoOp(rect_disp++); " + MARK),
    ]),

    # -- S4: audiomemory -----------------------------------------------------
    # SHADOW.  game64.c_inc is #included exactly once, from
    # src/static/jaudio_NES/game/game64.c:2 as
    # "../src/static/jaudio_NES/game/game64.c_inc" — same resolution story as
    # ac_npc_ctrl.c_inc, and VERIFIED the same way with -H.  Shadowing rather
    # than swapping also sidesteps game64.c's CXXFROMC_SRC per-TU flag rule,
    # which is keyed on the src/ path.
    ("src/static/jaudio_NES/game/game64.c_inc", "shadow", [
        (1,
         r"^u8 audiomemory\[0x90000\] ATTRIBUTE_ALIGN\(32\);$",
         "/* DC_SRC_SHRINK: 0x90000 -> 0x76000 (589,824 -> 483,328 B).  This heap is\n"
         " * handed whole to Jac_HeapSetup() and drawn down only through OSAlloc2().\n"
         " * Every consumer: dac 3x2240 (aictrl.c:62) + dspbuf 3x2240 (dspbuf.c:29)\n"
         " * + cpubuf 3x2240 (cpubuf.c:26) + tmp_buf 2240 (neosthread.c:88) +\n"
         " * acmdBuf 0x70000 (neosthread.c:92, AGC.acmdBufSize) = 481,152 B, leaving\n"
         " * 2,176 B of margin.  DO NOT SHRINK FURTHER — Nas_WaveDmaNew()\n"
         " * (internal/system.c:431) breaks out of its allocation loop silently on\n"
         " * exhaustion and drops voices with no diagnostic. */\n"
         "u8 audiomemory[0x76000] ATTRIBUTE_ALIGN(32);"),
    ]),

    # -- S5: sys_stacks ------------------------------------------------------
    # SHADOW of a header, and the one case where that is safe: sys_stacks.h is
    # #included by exactly two files, src/main.c and src/system/sys_stacks.c,
    # both of which are .c files under src/ with no sibling copy of the header,
    # so both resolve through -I.  VERIFIED with -H for both.
    #
    # Shrinking the MACROS rather than the arrays is what makes this safe:
    # src/main.c:55 does memset(padmgrStack, 0xEB, PADMGR_STACK_SIZE) and lines
    # 53/60/62 pass `stack + SIZE`.  Change the arrays alone and that memset
    # smashes 4,080 B of neighbouring .bss.
    ("include/sys_stacks.h", "shadow", [
        (1, r"^#define IRQMGR_STACK_SIZE 0x1000$",
         "/* DC_SRC_SHRINK: no thread ever starts on Dreamcast — osCreateThread2\n"
         " * (dc/src/dc_os.c:250) discards its stack argument and osStartThread\n"
         " * starts nothing, so not one byte of these three arrays is ever used as\n"
         " * a stack.  The MACROS shrink, not the arrays, so src/main.c's\n"
         " * memset(padmgrStack, 0xEB, PADMGR_STACK_SIZE) and its three\n"
         " * `stack + SIZE` expressions stay consistent.  16,384 B -> 48 B. */\n"
         "#define IRQMGR_STACK_SIZE 0x10"),
        (1, r"^#define PADMGR_STACK_SIZE 0x1000$", "#define PADMGR_STACK_SIZE 0x10"),
        (1, r"^#define GRAPH_STACK_SIZE 0x2000$",  "#define GRAPH_STACK_SIZE 0x10"),
    ]),
]

# Expected .bss delta, from `sh-elf-nm -S` on the clean non-stub ELF. Printed so
# a build that saves the wrong amount is obvious at a glance.
EXPECTED = [
    ("aSTR_overlay",        294912,   512),
    ("aINS_overlay",         21528,    72),
    ("aNPC_n_overlay",        2056,    24),
    ("aNPC_s_overlay",       16400,    48),
    ("aNPC_k_overlay",       36888,    72),
    ("aNPC_e_overlay",       20496,    48),
    ("aGYO_overlay",         30720,    32),
    ("prbuf",               614400,    32),
    ("audiomemory",         589824, 483328),
    ("graphStack",            8192,    16),
    ("padmgrStack",           4096,    16),
    ("irqmgrStack",           4096,    16),
]


class RuleError(Exception):
    pass


def apply_rules(text, rel, rules):
    """Apply every rule to one file. Hard-errors on any count mismatch."""
    for want, pattern, repl in rules:
        rx = re.compile(pattern, re.MULTILINE)
        got = len(rx.findall(text))
        if got != want:
            raise RuleError(
                "%s: pattern matched %d time(s), expected %d.\n"
                "    pattern: %s\n"
                "  A silent no-match is the whole reason this check exists: the\n"
                "  build would succeed, save nothing, and nobody would notice.\n"
                "  The vendored source has drifted — re-derive the rule, do not\n"
                "  relax the anchor." % (rel, got, want, pattern))
        text = rx.sub(lambda m: repl, text, count=want)
    return text


def main():
    ap = argparse.ArgumentParser(description="Build the DC_SRC_SHRINK tree.")
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    out_root = Path(args.out).resolve()

    # DC_SRC_SHRINK and DC_ASSET_STUB both swap sources per-TU, and dc/Makefile
    # applies them in sequence — a file claimed by both would lose one of the
    # two rewrites silently. They are disjoint today; make it impossible for
    # that to stop being true without anyone noticing.
    stub_list = REPO / "dc" / "build" / "stubsrc" / "stub.list"
    stubbed = set()
    if stub_list.exists():
        stubbed = {ln.strip() for ln in stub_list.read_text().splitlines() if ln.strip()}

    written = 0
    unchanged = 0
    swap_rel = []
    shadow_rel = []
    edits = 0

    for rel, kind, rules in RULES:
        src = REPO / rel
        if not src.exists():
            raise SystemExit("make_src_shrink: missing source %s" % src)
        if kind == "swap" and rel in stubbed:
            raise SystemExit(
                "make_src_shrink: %s is claimed by BOTH DC_SRC_SHRINK and\n"
                "  DC_ASSET_STUB. dc/Makefile swaps one source path per TU, so one\n"
                "  of the two rewrites would be dropped without an error. Merge the\n"
                "  two rewrites into a single generator before proceeding." % rel)

        text = src.read_text(encoding="utf-8", errors="surrogateescape")
        try:
            new_text = apply_rules(text, rel, rules)
        except RuleError as e:
            raise SystemExit("make_src_shrink: %s" % e)
        edits += len(rules)

        (swap_rel if kind == "swap" else shadow_rel).append(rel)

        if args.dry_run:
            continue
        dst = out_root / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        if dst.exists() and dst.read_text(
                encoding="utf-8", errors="surrogateescape") == new_text:
            unchanged += 1
            continue
        dst.write_text(new_text, encoding="utf-8", errors="surrogateescape")
        written += 1

    if not args.dry_run:
        out_root.mkdir(parents=True, exist_ok=True)
        # Only the per-TU SWAPS go in the list make reads; the shadows are
        # picked up through the include path, not by name.
        (out_root / "shrink.list").write_text("\n".join(sorted(swap_rel)) + "\n")

    if not args.quiet:
        before = sum(b for _, b, _ in EXPECTED)
        after = sum(a for _, _, a in EXPECTED)
        print("== DC_SRC_SHRINK tree ==")
        print("  out            : {}".format(out_root))
        print("  TU swaps       : {}".format(len(swap_rel)))
        for r in sorted(swap_rel):
            print("                   {}".format(r))
        print("  include shadows: {}".format(len(shadow_rel)))
        for r in sorted(shadow_rel):
            print("                   {}".format(r))
        print("  literal edits  : {}".format(edits))
        if not args.dry_run:
            print("  written        : {}  (unchanged {})".format(written, unchanged))
        print("  expected .bss  : {:,} -> {:,}  ({:+,} B)".format(
            before, after, after - before))
        print("  (dc/Makefile adds -16,384 more by dropping KOS's dcache")
        print("   walk buffer out of dc/src/dc_os.c — see DC_OS_TU_OPT there.)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
