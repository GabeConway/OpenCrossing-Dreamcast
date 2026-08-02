#!/usr/bin/env python3
"""
make_src_shrink.py — build the DC_SRC_SHRINK source tree (kb/STATE.md, step S3).

WHY THIS EXISTS
---------------
The image is ~8.8 MB over the 16 MB inequality and `.bss` is where the budget
has to come from.  A handful of numeric literals in the vendored decomp size
buffers that this port either never writes (dead code paths) or over-allocates
by a wide margin.  Rewriting those literals recovers 1,175,776 B with no
codegen change of any kind.  One later rule (S6) does the same job on the
image-span side: it deletes a dead .rodata string pool.

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

S6  s_assets[] name strings DELETED                             -598,424
    `pc/src/pc_assets.c` (kb/ram-plan.md P6).  This one is .rodata, not .bss.
    The 14,495 "assets/<name>.bin" literals (540,668 B once 4-aligned) and the
    `const char* path` slot that points at them (14,495 x 4 = 57,980 B) are
    both removed; the table's five live fields are untouched.  The strings had
    exactly one consumer — pc_load_asset()'s `.bin` fopen fallback — and that
    fallback is unreachable on Dreamcast.  Full argument at the rule.
    NOTE: kb/levers.md L3 and kb/ram-plan.md P6 say -821,569.  MEASURED WRONG
    by 223,145 B; 821,569 was pc_assets.c's whole .rodata contribution, which
    includes the 347,880 B s_assets table that is live and stays.

S5  sys_stacks 16,384->48                                        -16,336
    `include/sys_stacks.h`.  `osCreateThread2` on DC (`dc/src/dc_os.c:250`)
    ignores its stack argument entirely and `osStartThread` starts nothing, so
    no byte of these three arrays is ever used as a stack.  Shrinking the
    MACROS rather than the arrays keeps `src/main.c:55`'s
    `memset(padmgrStack, 0xEB, PADMGR_STACK_SIZE)` and the three
    `stack + SIZE` expressions consistent with the new sizes.

S7  data_bgd collision split (kb/ram-plan.md P7)                -246,064
    `.data`, not `.bss`.  `data_bgd[295]` is 317,420 B of `.data` and 302,080 B
    of that (95.2 %) is the `mCoBG_Collision_u collision[16][16]` member — a
    1 KB acre collision map per row.  It is read in exactly ONE place in the
    whole tree (`m_field_make.c:271`, `mFM_BgUtDataSet`), sequentially, once
    per block load, straight into the heap-resident `mFM_bg_info_c::collision`.
    So it never needs to be addressable as an array: run-length coding it and
    expanding at that one call site costs nothing but the decode.
    The member becomes a `const u8*` into a shared RLE stream.  See the rule
    for the encoding, the round-trip check, and why no header is shadowed.

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
# `replacement` is applied with re.MULTILINE and is used LITERALLY — it is fed
# to re.sub through a lambda, so `\1` and friends are NOT expanded.  When a rule
# needs the matched text, pass a CALLABLE instead: it receives the match object
# and returns the replacement string (see the S6 table rule).

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

    # -- S6: s_assets[] name strings (kb/ram-plan.md P6) ----------------------
    # SWAP.  pc/src/pc_assets.c is compiled directly (dc/Makefile PC_REUSE_C),
    # so -I cannot reach it, and both the PCAsset typedef and s_assets are
    # file-local (`static const`) — there is no second TU that could disagree
    # about the layout, so no ODR split is possible.  The assert on
    # sizeof(PCAsset) pins the struct the row rewrite was derived from.
    #
    # WHY THE STRINGS ARE DEAD ON DREAMCAST — verified, not assumed:
    #   * s_assets[i].path has exactly ONE consumer: it is handed to
    #     pc_load_asset() as `bin_path`, which uses it for (a) the `.bin`
    #     fallback `fopen(bin_path)` and (b) the "ASSET MISSING" message.
    #     Nothing indexes, hashes or compares the strings.
    #   * The fallback is unreachable on DC.  pc_disc_is_open()
    #     (dc/src/dc_dvd.c:279) returns 1 unconditionally, and _extract_dol /
    #     _extract_rel read /cd/main.dol and /cd/foresta.rel, so rom_mode is the
    #     only mode this port has.  All 14,495 rows carry rom_src 0 (DOL) or 1
    #     (REL) — NOT ONE is SRC_NONE — so `bin_path` is only ever reached when
    #     the ROM pointer itself is NULL, i.e. when the disc has no main.dol and
    #     the whole table has already failed.  Even then the fopen is dead: the
    #     paths are relative ("assets/<name>.bin"), KOS's cwd is "/", and
    #     nothing stages an /assets tree onto the disc (dc/stage-disc.sh
    #     flattens everything into the root that dc_dvd.c opens as /cd/<name>).
    #
    # So the rewrite deletes the field outright rather than replacing it with an
    # index into a disc-side name table: there is nothing left to look up.  The
    # table index takes over as the identifier in the diagnostic, and the
    # vendored pc/src/pc_assets.c is the index -> name map for anyone debugging.
    #
    # MEASURED with sh-elf-size -A on the object, before -> after:
    #   .rodata (the string pool)  540,973 -> 305      -540,668
    #   .rodata.s_assets           347,880 -> 289,900   -57,980  (14,495 x 4 B)
    #   .text.pc_load_asset_dcshrink     0 -> 236           +236
    #
    # MEASURED on the LINKED IMAGE, two clean full rebuilds of all 3917 TUs
    # differing only in whether this rule ran (2026-08-02):
    #   .rodata   1,057,364 -> 458,716    -598,648
    #   .text     5,283,456 -> 5,283,680      +224
    #   .data / .bss                       unchanged
    #   image     19,824,552 -> 19,226,128 -598,424
    #   span      0x12e81c0 -> 0x1255f60   -598,112
    #   strings matching "^assets/" in the ELF: 16,365 -> 1,870 (exactly the
    #   14,495 table rows; the 1,870 left are the per-TU _pc_load_src_* call
    #   sites in src/, a different and much smaller pool).
    #
    # kb/levers.md L3 and kb/ram-plan.md P6 both claim -821,569 B.  That is
    # WRONG by 223,145 B: 821,569 was derived from pc_assets.c's TOTAL .rodata
    # contribution (888,853 B), which is the string pool PLUS the 347,880 B
    # s_assets table itself — and the table is live and stays.
    ("pc/src/pc_assets.c", "swap", [
        # 1. Drop `path` from the struct, and add the indexed loader that
        #    replaces the one call site that used it.
        (1,
         r"^typedef struct \{ const char\* path; void\* dest; unsigned int size; "
         r"unsigned int rom_off; int rom_src; int swap; \} PCAsset;$",
         "/* " + "-" * 68 + "\n"
         " * DC_SRC_SHRINK S6 (kb/ram-plan.md P6): the `path` field and its\n"
         " * 14,495 \"assets/<name>.bin\" string literals are DELETED — 540,668 B\n"
         " * of .rodata string pool plus 57,980 B of pointer slots.\n"
         " *\n"
         " * They were only ever used by pc_load_asset()'s .bin fallback, which\n"
         " * cannot be reached on Dreamcast: every row is rom_src DOL or REL,\n"
         " * the ROM-direct path is the only mode dc/src/dc_dvd.c implements,\n"
         " * and the relative \"assets/...\" paths resolve against KOS's \"/\" cwd\n"
         " * where nothing is staged.  The table INDEX is the identifier now;\n"
         " * map it back through the vendored pc/src/pc_assets.c.\n"
         " * " + "-" * 68 + " */\n"
         "typedef struct { void* dest; unsigned int size; unsigned int rom_off;"
         " int rom_src; int swap; } PCAsset;\n"
         + ASSERT("pc_assets_row", "sizeof(PCAsset) == 5 * sizeof(int)") + "\n"
         "\n"
         "/* The table's loader.  Same semantics as pc_load_asset() minus the\n"
         " * dead fopen() fallback; the diagnostic names the row by index and\n"
         " * stays loud, because a silent miss here is a garbage asset. */\n"
         "static void pc_load_asset_dcshrink(int idx, void* dest, "
         "unsigned int size,\n"
         "                                   unsigned int rom_off, int rom_src,\n"
         "                                   int swap_type) {\n"
         "    u8* rom = (rom_src == SRC_REL) ? g_rel_data\n"
         "            : (rom_src == SRC_DOL) ? g_dol_data\n"
         "            : (u8*)NULL;\n"
         "    if (rom == NULL) {\n"
         "        fprintf(stderr, \"[PC] ASSET MISSING: s_assets[%d] "
         "(rom_src=%d off=0x%X size=%u)\\n\",\n"
         "                idx, rom_src, rom_off, size);\n"
         "        return;\n"
         "    }\n"
         "    memcpy(dest, rom + rom_off, size);\n"
         "    do_swap(dest, size, swap_type);\n"
         "}"),

        # 2. The 14,495 rows.  Anchored on the exact generated shape:
        #    four spaces, '{', a double-quoted path, ', '.  MEASURED: this
        #    matches 14,495 lines in the whole file and nothing else, so the
        #    count below is the file's own checksum.  The remaining five
        #    initialisers keep their positions in the shortened struct.
        (14495, r'^    \{"[^"]*", ', lambda m: "    {"),

        # 3. The one consumer of .path.
        (1,
         r"^        pc_load_asset\(s_assets\[i\]\.path, s_assets\[i\]\.dest, "
         r"s_assets\[i\]\.size,\n"
         r"                      s_assets\[i\]\.rom_off, s_assets\[i\]\.rom_src, "
         r"s_assets\[i\]\.swap\);$",
         "        pc_load_asset_dcshrink(i, s_assets[i].dest, s_assets[i].size,\n"
         "                               s_assets[i].rom_off, "
         "s_assets[i].rom_src,\n"
         "                               s_assets[i].swap); " + MARK),
    ]),
]

# ---------------------------------------------------------------------------
# S7 — the data_bgd collision split (kb/ram-plan.md P7).
# ---------------------------------------------------------------------------
# Unlike S1-S6 this rule cannot be spelled as a literal, because the
# replacement text IS the re-encoded data.  It is generated from the vendored
# source at run time, so the two rules below are built by _s7_rules() and
# appended to RULES.
#
# WHAT IS BEING MOVED, AND WHY IT IS SAFE
# ---------------------------------------
# `mFM_bg_data_c` (include/m_field_make.h:269) carries
# `mCoBG_Collision_u collision[UT_Z_NUM][UT_X_NUM]` — 16 x 16 x 4 = 1,024 B per
# acre.  `data_bgd[295]` is therefore 317,420 B of .data of which 302,080 B is
# collision.  MEASURED against the linked ELF, not asserted: the .data.data_bgd
# input section is 0x4d7ec = 317,420 B and data_bgd_number reads 295, so
# sizeof(mFM_bg_data_c) == 1076 exactly.
#
# The member has exactly ONE reader in the entire tree:
#
#     m_field_make.c:271
#       mFM_BgUtDataSet(bg_info->collision[0], bg_info->keep_h[0],
#                       bg_data->collision[0]);
#
# and mFM_BgUtDataSet (m_field_make.c:121) walks it strictly forward, 256 units,
# copying each into the heap-resident mFM_bg_info_c::collision and its low 5
# bits into keep_h.  Nothing indexes it randomly, takes its address, memcpy()s
# it, or byte-swaps it — the TARGET_PC bswap path in that file touches only FG
# data loaded from ARAM (mFM_ByteSwapFGData), never this compile-time table.
# So the storage does not have to be an array at all; it only has to be
# replayable in order, once, at that call site.
#
# THE ENCODING
# ------------
#   * palette: the distinct unit initialisers, ordered by descending frequency.
#     380 of them across all 75,520 units.  Emitted as C initialiser TEXT, so
#     the COMPILER packs the bitfields — this generator never needs to know the
#     bit layout of mCoBG_CollisionData_c, and cannot get it wrong.
#   * stream: per-array RLE.  Each run is  u8 len (1..255)  then a palette index
#     as a varint: one byte when < 0x80, otherwise 0x80|(idx>>8) then idx&0xFF.
#     97.6 % of runs take the one-byte form.
#   * arrays are deduplicated first (295 -> 257 distinct), so acres that share a
#     collision map share a stream.
#
# WHY TEXT IDENTITY IS A SOUND KEY: cross-checked against the linked ELF —
# the 380 distinct unit TEXTS map onto exactly 380 distinct u32 VALUES, one to
# one, in parse order across all 75,520 units.  (Had two texts collided onto one
# value the only cost would be a slightly larger palette; the reverse cannot
# happen, since the same text compiles to the same bytes.)
#
# WHY NO HEADER SHADOW.  m_field_make.h is #included by 61 TUs *and by ten other
# headers inside include/*, so a shadow could never reach all of them — the same
# half-applied split that forced S1a to be a per-TU swap.  Instead both TUs that
# name the type get a local twin and a pair of #defines placed AFTER the
# includes, so the vendored header keeps its own type and its own (now
# unreferenced) `extern mFM_bg_data_c data_bgd[];` declaration.  The twin is
# emitted from ONE string constant into both files, so they cannot disagree, and
# each file asserts sizeof() on both the twin and the vendored original.
#
# ROUND TRIP: the generator decodes its own stream with the same algorithm the C
# decoder uses and hard-errors unless it reproduces all 75,520 unit texts.  A
# corrupt collision map would show up as invisible walls in the field, which is
# exactly the sort of failure that would be blamed on something else for a week.
#
# MEASURED ON THE LINKED IMAGE, two clean full rebuilds of all 3917 TUs differing
# only in whether this rule ran (2026-08-02, non-stub, DC_ARAM_WINDOW=851968
# DC_ARENA_BYTES=1900000):
#   .data       2,638,872 -> 2,337,976   -300,896   (data_bgd 317,420 -> 16,520)
#   .text col   5,749,148 -> 5,803,980    +54,832   (the column carries .rodata:
#                                                    palette 1,520 + stream
#                                                    53,150 + decoder 162)
#   .bss       11,145,696 -> 11,145,696         0
#   image dec  19,533,716 -> 19,287,652   -246,064
#   span      0x12a12e0 -> 0x1265100      -246,240
#
# CHECKED, not assumed, on the linked ELF:
#   * the palette and stream were read back out of the ELF, decoded with this
#     same algorithm, and compared against the previous build's 295 x 1,024 B of
#     data_bgd[].collision — bit for bit identical, all 295 maps;
#   * `data_bgd` is ABSENT from the ELF with no dangling U reference, which also
#     proves it had exactly one definition (it is NOT one of the 1,367
#     multiply-defined symbols, despite "collision" in the kb's name for it);
#   * each of data_bgd_dcshrink / mFM_bgcol_pal_dcshrink / mFM_bgcol_enc_dcshrink
#     / data_bgd_number has exactly ONE defining input section in the map, and
#     _graph_proc still resolves exactly once;
#   * DC_SRC_SHRINK=0 puts .data back to 2,638,872 exactly.
#
# kb/levers.md L3 and kb/ram-plan.md P7 both claimed -236,544 and called it
# ".bss". It is .data, and the real figure is 9,520 B BETTER — the first row in
# that plan to beat its estimate rather than miss it. Plain dedup of identical
# collision maps, which is what "-236,544" most plausibly meant, was measured at
# only 38,912 B (295 arrays, 257 distinct): the saving is in the run-length
# structure, not in duplicate acres.

S7_BGD = "src/data/field/bg/acre/bg_data.c"
S7_FM = "src/game/m_field_make.c"
S7_UNITS = 256              # UT_Z_NUM * UT_X_NUM
S7_ENTRIES = 295            # data_bgd_number; asserted below

# The twin type, emitted verbatim into BOTH TUs so they cannot drift apart.
# Field order and types are copied from include/m_field_make.h:269-279 with the
# collision array replaced by a pointer; everything else is byte-identical, so
# every positional initialiser in bg_data.c keeps its slot.
S7_TWIN = """\
/* --------------------------------------------------------------------
 * DC_SRC_SHRINK S7 (kb/ram-plan.md P7): mFM_bg_data_c::collision[16][16]
 * — 1,024 B x 295 acres = 302,080 B of .data — is replaced by a pointer
 * into a shared run-length stream.  Its one reader, mFM_BgUtDataSet(),
 * expands it at the single call site in m_field_make.c.  The vendored
 * type in include/m_field_make.h is left alone; this twin and the two
 * #defines below are confined to this TU, so no other TU can disagree
 * about a layout it never sees.
 * -------------------------------------------------------------------- */
typedef struct {
    mActor_name_t bg_id;
    Gfx* opaque_gfx;
    Gfx* translucent_gfx;
    EVW_ANIME_DATA* animation;
    s8 animation_count;
    u32 rom_start_addr;
    u32 rom_end_addr;
    const u8* collision_enc;
    mFM_bg_sound_source_data_c sound_source[mFM_SOUND_SOURCE_NUM];
} mFM_bg_data_dcshrink_c;
""" + ASSERT("bgd_vendored", "sizeof(mFM_bg_data_c) == 1076") + "\n" \
    + ASSERT("bgd_twin", "sizeof(mFM_bg_data_dcshrink_c) == 56") + "\n" \
    + ASSERT("bgd_unit", "sizeof(mCoBG_Collision_u) == 4") + """
#define mFM_bg_data_c mFM_bg_data_dcshrink_c
#define data_bgd      data_bgd_dcshrink
"""

# One collision block as it is generated into bg_data.c: the comment, the
# opening brace, exactly 16 row lines, the closing brace-comma.  Anchored to
# the column so it cannot match the sound-source block or anything nested.
S7_BLOCK_RX = (r"^        // collision data\n"
               r"        \{\n"
               r"(?:            \{.*\},\n){16}"
               r"        \},$")

S7_UNIT_RX = re.compile(r"\{[^{}]*\}")

S7_STATS = {}


def _s7_parse(text):
    """Pull the 295 collision blocks out of bg_data.c as (block_text, units)."""
    out = []
    for m in re.finditer(S7_BLOCK_RX, text, re.MULTILINE):
        rows = m.group(0).split("\n")[2:-1]
        units = []
        for r in rows:
            u = S7_UNIT_RX.findall(r)
            if len(u) != 16:
                raise SystemExit("make_src_shrink S7: a collision row has %d "
                                 "units, expected 16:\n  %s" % (len(u), r[:120]))
            units += u
        if len(units) != S7_UNITS:
            raise SystemExit("make_src_shrink S7: block has %d units, expected %d"
                             % (len(units), S7_UNITS))
        out.append((m.group(0), tuple(units)))
    return out


def _s7_encode(arrays, index_of):
    """RLE one array into (u8 len, varint palette index) runs."""
    buf = bytearray()
    for a in arrays:
        i = 0
        while i < S7_UNITS:
            j = i
            while j < S7_UNITS and a[j] == a[i] and j - i < 255:
                j += 1
            k = index_of[a[i]]
            buf.append(j - i)
            if k < 0x80:
                buf.append(k)
            else:
                buf.append(0x80 | (k >> 8))
                buf.append(k & 0xFF)
            i = j
    return bytes(buf)


def _s7_decode(stream, off, palette):
    """The C decoder, in Python. Must stay in lockstep with mFM_BgUtDataSet."""
    out = []
    n = S7_UNITS
    p = off
    while n > 0:
        ln = stream[p]; p += 1
        k = stream[p]; p += 1
        if k & 0x80:
            k = ((k & 0x7F) << 8) | stream[p]; p += 1
        if ln == 0 or ln > n:
            raise SystemExit("make_src_shrink S7: malformed run len=%d n=%d" % (ln, n))
        out += [palette[k]] * ln
        n -= ln
    return tuple(out)


def _s7_rules():
    import collections

    text = (REPO / S7_BGD).read_text(encoding="utf-8", errors="surrogateescape")
    blocks = _s7_parse(text)
    if len(blocks) != S7_ENTRIES:
        raise SystemExit(
            "make_src_shrink S7: found %d collision blocks in %s, expected %d.\n"
            "  The vendored table has changed size — re-derive the rule (and the\n"
            "  sizeof asserts in S7_TWIN) rather than relaxing this check."
            % (len(blocks), S7_BGD, S7_ENTRIES))

    freq = collections.Counter(u for _, units in blocks for u in units)
    palette = [u for u, _ in freq.most_common()]
    index_of = {u: i for i, u in enumerate(palette)}
    if len(palette) > 0x8000:
        raise SystemExit("make_src_shrink S7: palette of %d exceeds the varint's "
                         "15-bit index" % len(palette))

    # Deduplicate whole arrays, first-seen order, and lay their streams out
    # back to back.
    offsets = {}
    order = []
    for _, units in blocks:
        if units not in offsets:
            offsets[units] = None
            order.append(units)
    stream = bytearray()
    for units in order:
        offsets[units] = len(stream)
        stream += _s7_encode([units], index_of)
    stream = bytes(stream)

    # ROUND TRIP — every one of the 295 arrays, through the same algorithm the
    # generated C runs. A silent encoder bug here is invisible walls in-game.
    for _, units in blocks:
        if _s7_decode(stream, offsets[units], palette) != units:
            raise SystemExit("make_src_shrink S7: round trip FAILED — refusing "
                             "to emit a collision stream that does not decode "
                             "back to the vendored table.")

    # ---- the emitted C ----------------------------------------------------
    pal_c = ["/* DC_SRC_SHRINK S7: the %d distinct collision units, most common"
             " first.\n * Emitted as initialiser TEXT so the compiler packs the"
             " bitfields — this\n * generator never has to know the bit layout of"
             " mCoBG_CollisionData_c. */" % len(palette),
             "const mCoBG_Collision_u mFM_bgcol_pal_dcshrink[%d] = {" % len(palette)]
    pal_c += ["    %s," % u for u in palette]
    pal_c.append("};")

    hexed = ["0x%02X," % b for b in stream]
    rows = ["    " + "".join(hexed[i:i + 16]) for i in range(0, len(hexed), 16)]
    enc_c = ["/* DC_SRC_SHRINK S7: %d acre collision maps (%d distinct) as"
             " run-length pairs:\n * u8 run length 1..255, then a palette index"
             " — one byte if < 0x80,\n * else 0x80|(idx>>8) followed by idx&0xFF."
             "  %d B, down from %d. */"
             % (S7_ENTRIES, len(order), len(stream), S7_ENTRIES * 1024),
             "static const u8 mFM_bgcol_enc_dcshrink[%d] = {" % len(stream)]
    enc_c += rows
    enc_c.append("};")

    preamble = "\n".join(pal_c) + "\n\n" + "\n".join(enc_c) + "\n\n"

    def block_repl(m):
        return "        &mFM_bgcol_enc_dcshrink[%d]," % offsets[
            _s7_parse(m.group(0))[0][1]]

    S7_STATS.update(
        entries=S7_ENTRIES, distinct=len(order), palette=len(palette),
        stream=len(stream),
        before=S7_ENTRIES * 1076,
        after=S7_ENTRIES * 56 + len(stream) + len(palette) * 4,
    )

    bgd_rules = [
        # 1. The twin type + the two #defines, after the file's one include.
        (1, r'^#include "m_field_info.h"$',
         '#include "m_field_info.h"\n\n' + S7_TWIN),
        # 2. The palette and the stream, immediately before the table that
        #    points into them.
        (1, r"^extern mFM_bg_data_c data_bgd\[\] = \{$",
         preamble + "extern mFM_bg_data_c data_bgd[] = {"),
        # 3. The 295 collision blocks -> a pointer into the stream. The count
        #    is the file's own checksum: this pattern matches the generated
        #    shape and nothing else.
        (S7_ENTRIES, S7_BLOCK_RX, block_repl),
    ]

    fm_rules = [
        # 1. Same twin, after the last include and before the first use. It has
        #    to sit after them: the vendored header must keep its own type.
        (1, r'^#include "m_bgm.h"$',
         '#include "m_bgm.h"\n\n' + S7_TWIN
         + "extern mFM_bg_data_dcshrink_c data_bgd_dcshrink[];\n"
           "extern const mCoBG_Collision_u mFM_bgcol_pal_dcshrink[];\n"),
        # 2. The one reader, replaced by the stream decoder. Same signature
        #    shape, same two outputs, same order.
        (1,
         r"^static void mFM_BgUtDataSet\(mCoBG_Collision_u\* collision, u8\* keep, "
         r"mCoBG_Collision_u\* data\) \{\n"
         r"    int ut_x;\n"
         r"    int ut_z;\n"
         r"\n"
         r"    for \(ut_z = 0; ut_z < UT_Z_NUM; ut_z\+\+\) \{\n"
         r"        for \(ut_x = 0; ut_x < UT_X_NUM; ut_x\+\+\) \{\n"
         r"            collision\[0\] = data\[0\];\n"
         r"            keep\[0\] = data\[0\]\.data\.center;\n"
         r"\n"
         r"            collision\+\+;\n"
         r"            data\+\+;\n"
         r"            keep\+\+;\n"
         r"        \}\n"
         r"    \}\n"
         r"\}$",
         "/* DC_SRC_SHRINK S7: `data` is now a run-length stream, not a\n"
         " * mCoBG_Collision_u[16][16].  Same two outputs in the same order —\n"
         " * the unit into collision[], its `center` field into keep[].\n"
         " * Encoding: u8 run length, then a palette index, one byte if < 0x80\n"
         " * else 0x80|(idx>>8) followed by idx&0xFF.  Generated and round-trip\n"
         " * checked by tools/dcstub/make_src_shrink.py.  The two defensive\n"
         " * bounds below cannot fire on a stream this generator emitted; they\n"
         " * are here so a future encoder bug overruns nothing. */\n"
         "static void mFM_BgUtDataSet(mCoBG_Collision_u* collision, u8* keep, "
         "const u8* data) {\n"
         "    int n = UT_Z_NUM * UT_X_NUM;\n"
         "\n"
         "    while (n > 0) {\n"
         "        int len = data[0];\n"
         "        int idx = data[1];\n"
         "        mCoBG_Collision_u unit;\n"
         "\n"
         "        data += 2;\n"
         "        if (idx & 0x80) {\n"
         "            idx = ((idx & 0x7F) << 8) | data[0];\n"
         "            data++;\n"
         "        }\n"
         "        if (len == 0) {\n"
         "            break;\n"
         "        }\n"
         "        if (len > n) {\n"
         "            len = n;\n"
         "        }\n"
         "        unit = mFM_bgcol_pal_dcshrink[idx];\n"
         "        n -= len;\n"
         "\n"
         "        while (len > 0) {\n"
         "            collision[0] = unit;\n"
         "            keep[0] = unit.data.center;\n"
         "\n"
         "            collision++;\n"
         "            keep++;\n"
         "            len--;\n"
         "        }\n"
         "    }\n"
         "}"),
        # 3. The one call site.
        (1,
         r"^    mFM_BgUtDataSet\(bg_info->collision\[0\], bg_info->keep_h\[0\], "
         r"bg_data->collision\[0\]\);$",
         "    mFM_BgUtDataSet(bg_info->collision[0], bg_info->keep_h[0],\n"
         "                    bg_data->collision_enc); " + MARK),
    ]

    return [(S7_BGD, "swap", bgd_rules), (S7_FM, "swap", fm_rules)]


RULES += _s7_rules()

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

# S6 is .rodata + a hair of .text, not .bss, so it is reported separately.
# Measured with sh-elf-size -A on pc_assets.c.o built with the Makefile's exact
# PC_REUSE flag set, before and after.
EXPECTED_RODATA = [
    (".rodata (s_assets path string pool)", 540973,    305),
    (".rodata.s_assets (the 14,495 rows)",  347880, 289900),
]
EXPECTED_TEXT_S6 = 224        # pc_load_asset_dcshrink, as it lands in the linked .text


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
        fn = repl if callable(repl) else (lambda m: repl)
        text = rx.sub(fn, text, count=want)
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
        rb = sum(b for _, b, _ in EXPECTED_RODATA)
        ra = sum(a for _, _, a in EXPECTED_RODATA)
        print("  expected .rodata: {:,} -> {:,}  ({:+,} B)   [S6, pc_assets.c]".format(
            rb, ra, ra - rb))
        print("  expected .text : {:+,} B   [S6's indexed loader]".format(
            EXPECTED_TEXT_S6))
        s = S7_STATS
        print("  expected .data : {:,} -> {:,}  ({:+,} B)   [S7, data_bgd]".format(
            s["before"], s["after"], s["after"] - s["before"]))
        print("                   {:,} acres, {:,} distinct maps, palette {:,},"
              " stream {:,} B".format(
                  s["entries"], s["distinct"], s["palette"], s["stream"]))
        print("  (dc/Makefile adds -16,384 more by dropping KOS's dcache")
        print("   walk buffer out of dc/src/dc_os.c — see DC_OS_TU_OPT there.)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
