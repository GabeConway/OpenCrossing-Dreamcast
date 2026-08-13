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
    `pc/src/pc_assets.c` (kb/levers.md P6).  This one is .rodata, not .bss.
    The 14,495 "assets/<name>.bin" literals (540,668 B once 4-aligned) and the
    `const char* path` slot that points at them (14,495 x 4 = 57,980 B) are
    both removed; the table's five live fields are untouched.  The strings had
    exactly one consumer — pc_load_asset()'s `.bin` fopen fallback — and that
    fallback is unreachable on Dreamcast.  Full argument at the rule.
    NOTE: kb/levers.md L3 and kb/levers.md P6 say -821,569.  MEASURED WRONG
    by 223,145 B; 821,569 was pc_assets.c's whole .rodata contribution, which
    includes the 347,880 B s_assets table that is live and stays.

S5  sys_stacks 16,384->48                                        -16,336
    `include/sys_stacks.h`.  `osCreateThread2` on DC (`dc/src/dc_os.c:250`)
    ignores its stack argument entirely and `osStartThread` starts nothing, so
    no byte of these three arrays is ever used as a stack.  Shrinking the
    MACROS rather than the arrays keeps `src/main.c:55`'s
    `memset(padmgrStack, 0xEB, PADMGR_STACK_SIZE)` and the three
    `stack + SIZE` expressions consistent with the new sizes.

S9  jaudio DMA staging buffers          UNCONDITIONAL           -49,152
    Both halves are the same shape: a fixed-size staging buffer whose one
    consumer clamps its burst to the SAME macro, so shrinking the buffer only
    makes the loop iterate more.  Correct at DC_AUDIO=1 as well, which is why
    this pair is not keyed on the audio flag.
    S9a heapctrl.c DMABUFFER_SIZE 0x10000->0x1000.  MEASURED SAVING: ZERO —
        `.bss.dmabuffer` is ALREADY dropped by --gc-sections (dc/build/
        gcdrop.txt), because its only reachable user Jac_GarbageCollection_St
        is dropped too.  Kept anyway; see the rule.
    S9b dvdthread.c dvd_buf 0x10000->0x4000 + __WriteBufferSize(...,2,0x8000)
        -> (...,2,0x2000).  -49,152.  BOTH OR NEITHER.

S8  jaudio pools, ONLY when --audio=0                          -401,216
    seq 275,456->8,608 · FREE_SEQP_QUEUE 1,024->32 · CHANNEL 81,920->2,560 ·
    CALLSTACK 32,768->4,096 · pc_task_buf 25,600->256
    These are the sequencer's track pool, the logical-channel pool, the DVD
    task-frame ring and the RSP command buffer.  They are live and must not be
    touched when sound is on.  With DC_AUDIO=0 the per-frame chain that fills
    them is severed at its root — see THE S8 ARGUMENT below.

S10 acre ground textures off the disc (R1)                     -150,880 of keep
    `src/game/m_field_make.c`. Not a shrink at all — a CALL-SITE REDIRECT, here
    only because this tree is the one seam that reaches that TU without editing
    `src/` (mFM_LoadBGCommonTex, its six segment tables, l_bg_tex_common_dummy
    and l_water_permission are all `static`, so --wrap cannot see them; and
    `bcopy` is not a seam either — it is undeclared for this target and GCC
    folds it to memmove).
    The single `bcopy(bg_tex_tbl[i], …)` in that function becomes
    `dc_bgtex_load(bg_tex_tbl[i], …)`, which reads the bytes straight off the
    disc into the staging buffer that is already resident. The 96 mFM_grd_*
    source arrays (150,880 B: 46 summer / 41 winter / 9 shared) then never need
    keeping — which is worth 80,736 B of keep list today AND is what stops the
    winter town's ground rendering black, since the keep list could only ever
    afford one season. dc/src/dc_bgtex.c carries the whole argument;
    tools/dcstub/make_stub_data.py emits the pointer -> ROM map it looks up in.
    Kill switch: --bgtex-demand=0, which must match dc/Makefile's
    DC_BGTEX_DEMAND and make_stub_data.py's own flag. The rewritten TU carries
    an #error for the mismatch, in both directions.

S11 villager textures out of a 16-slot pool (R2)               -993,984 of keep
    `src/actor/npc/ac_npc_ctrl.c_inc` (shadow), `src/game/m_start_data_init.c`
    and `src/game/m_trademark.c` (swaps). Not a shrink either — two inserted
    calls, no code deleted. `src/data/npc/model/tex/*.c` is 1,154,944 B of
    .bss, 993,984 B of it the 236 VILLAGER sets, and a town holds at most 15
    villagers plus an islander. So `dc_npctex_ensure()` goes into
    `aNPC_dma_draw_data_proc` — the one choke point every texture snapshot
    passes through — and reads the wanted set off the disc into one of 16
    static slots, and `dc_npctex_pool_reset()` goes into the only two
    npclist-rebuilding sites that are provably free of live NPC actors.
    Worth 90,464 B of keep list today AND it is what makes 215 villager
    species stop rendering untextured. The special NPCs (rows >= ALL_NPC_NUM —
    Tom Nook, Rover, K.K., Porter, the raccoons) are NOT pooled and stay on
    the keep list. dc/src/dc_npctex.c carries the whole argument, including
    why --wrap=mNpc_SetNpcList would have been a silent no-op.
    Kill switch: --npctex-pool=0, matching dc/Makefile's DC_NPCTEX_POOL and
    make_stub_data.py's own flag. The rewritten TUs carry an #error for the
    mismatch, in both directions.

S12 villager models out of a 16-slot pool (R3)                  +115,296 .bss
    The same three TUs as S11, riding the same three seams — two more inserted
    calls, no code deleted. `src/data/npc/model/mdl/*.c` is 438,640 B of .bss
    in 72 species, of which 32 (194,400 B) are reachable from a VILLAGER row of
    npc_draw_data_tbl[] and 40 (244,240 B) only from a SPECIAL row; the two
    sets are disjoint, which is what lets R3 pool by skeleton pointer and still
    be unable to touch Tom Nook. `dc_npcmdl_ensure()` goes in beside
    `dc_npctex_ensure()` and `dc_npcmdl_pool_reset()` beside
    `dc_npctex_pool_reset()`.
    ⚠️ THIS ONE COSTS BYTES. Only cbr_1 (5,536 B) of the 32 was ever resident,
    so R3 is a content restoration: 120,832 B of pool for 31 villager species
    that drew as a black spiky mess no matter how good R2's texture was. The
    comparison that justifies the POOL is against keeping all 32 (194,400 B),
    not against today.
    Unlike every other rule here it needs RELOCATION: 933 `gsSPVertex(&<sp>_v[N])`
    words are R_SH_DIR32 relocations the linker baked into .data, and
    emu64::seg2k0 passes an 0x8Cxxxxxx address through untouched. dc/src/
    dc_npcmdl.c carries the whole argument, including why the patch table is
    generated rather than walked at runtime.
    Kill switch: --npcmdl-pool=0, matching dc/Makefile's DC_NPCMDL_POOL and
    make_stub_data.py's own flag. The rewritten TUs carry an #error for the
    mismatch, in both directions.

N3  the villager-construction diagnostic                        0 BYTES
    `src/actor/ac_set_npc_manager.c` (swap), `src/actor/npc/ac_npc_cloth.c_inc`
    and `src/actor/npc/ac_npc_ctrl.c_inc` (shadows). NOT A SHRINK AND NOT A
    FEATURE — an instrument, off by default, that answers "why is no villager
    actor ever constructed" in ONE Flycast town run instead of five.
    The roster is already known good ([DC/NPCSEED] pre ids=6 homed=6 | seeded
    id=8 home=8), so the break is downstream, and the chain from there is five
    functions long with nine serial gates that say nothing when they refuse.
    Every one of them gets wrapped in `dc_npcdiag_gate(SLOT, <predicate>)`,
    which returns its second argument untouched — no branch changes shape and
    every `&&` keeps its short-circuit order. dc/src/dc_npcdiag.c carries the
    five hypotheses and the field on the printed line that kills each.
    ⚠️ Its gate slot NUMBERS are parsed out of dc/include/dc_npcdiag.h rather
    than restated: two copies of an enum compared only by number would relabel
    a counter without breaking a build.
    Kill switch: --npcdiag=0, matching dc/Makefile's DC_NPCDIAG. Unlike R1/R2/R3
    the #error guard is emitted AT 0 AS WELL, because a mismatched tree here
    prints a diagnostic line of zeroes and zero is a meaningful reading.

S7  data_bgd collision split (kb/levers.md P7)                -246,064
    `.data`, not `.bss`.  `data_bgd[295]` is 317,420 B of `.data` and 302,080 B
    of that (95.2 %) is the `mCoBG_Collision_u collision[16][16]` member — a
    1 KB acre collision map per row.  It is read in exactly ONE place in the
    whole tree (`m_field_make.c:271`, `mFM_BgUtDataSet`), sequentially, once
    per block load, straight into the heap-resident `mFM_bg_info_c::collision`.
    So it never needs to be addressable as an array: run-length coding it and
    expanding at that one call site costs nothing but the decode.
    The member becomes a `const u8*` into a shared RLE stream.  See the rule
    for the encoding, the round-trip check, and why no header is shadowed.

THE S8 ARGUMENT — why these pools are dead at DC_AUDIO=0, and only then
-----------------------------------------------------------------------
⚠️ An earlier revision of this file said `CALLSTACK[0x8000]` and
`pc_task_buf[2][1600]` "are NOT dead scaffolding … shrinking either corrupts
audio silently".  That is STILL TRUE with sound on, and it is why every S8 rule
is keyed on `--audio=0` and why each rewritten TU carries an `#error` guard.
What that note missed is that `DC_AUDIO=0` does not merely mute the output — it
compiles out the only thing that drives the whole engine.

The chain, re-verified at file:line 2026-08-04:

  * `dc_audio_pump()` (`dc/src/dc_audio.c:497`) has its ENTIRE body inside
    `#if DC_AUDIO && !defined(DC_HOST_STUB)`.  At DC_AUDIO=0 it is an empty
    function.
  * that body holds the port's only call to `pc_audio_process_frame()`
    (`:557`).  The other caller in the tree, `pc/src/pc_audio.c:43`, is the SDL
    producer thread and is not in the DC build.
  * `pc_audio_process_frame()` (`audiothread.c:92`) is the only caller of
    `Jac_UpdateDAC()` outside the GameCube `#else` block (`audiothread.c:108`
    vs `:215`).
  * `Jac_UpdateDAC()` (`aictrl.c:285`) is the only caller of `Jac_VframeWork()`
    (`aictrl.c:299`), which is what ticks the sequencer, and the only path to
    `MixCpu` -> `CpubufProcess(MIX)` -> `CpubufProcess(FRAME_END)` ->
    `Neos_Update()` (`cpubuf.c:49`, its only call site) ->
    `CreateAudioTask(pc_task_buf[cur], …)` (`neosthread.c:35`).

So with sound off: no sequencer tick, no note-on, no `CreateAudioTask`.  What
DOES still run is INITIALISATION — `StartAudioThread()` is called
unconditionally (`verysimple.c:10`) and sets these pools up.  That is exactly
why the pools are shrunk rather than deleted: `Jaq_Reset`, `InitGlobalChannel`
and `jac_dvdproc_init` still execute and still walk them, and they walk them
with the SAME macro that sizes them.

The residual behaviour, stated plainly rather than hidden: game logic can still
ASK for a sequence (`Jaq_SetSeqData` -> `GetNewTrack`), and with the tick dead
nothing ever hands a track back, so the pool drains and stays drained.  Every
allocation site checks — `GetNewTrack` returns NULL and both callers test it
(`seqsetup.c:369`, `:509`), `FixAllocChannel` breaks out of its loop on a dry
free list (`driverinterface.c:210`), `BackTrack` refuses to overfill
(`seqsetup.c:77`) — so a drained pool is a silent no-op, not a corruption.  At
256 entries the leak was simply slower.  The audio subsystem at DC_AUDIO=0 is
already non-functional in exactly this way (see the `[TRG_SE] NO FREE` note in
`dc/src/dc_audio.c:487`); S8 does not create that condition, it declines to pay
390 KB of `.bss` for it.

USAGE
-----
    python3 tools/dcstub/make_src_shrink.py [--out DIR] [--audio 0|1]
                                            [--bgtex-demand 0|1]
                                            [--npctex-pool 0|1]
                                            [--npcmdl-pool 0|1]
                                            [--npc-seed 0|1|2]
                                            [--npcdiag 0|1]
                                            [--dry-run] [--quiet]

Default --out is dc/build/shrinksrc.  Files are written only when their content
actually changes, so re-running does not invalidate objects.

`--audio` MUST match the build's `DC_AUDIO`.  `dc/build-dc.sh` forwards it, and
every S8-rewritten TU carries an `#error` that fires if a tree generated with
`--audio=0` is compiled with `-DDC_AUDIO=1` — without it, a stale
`dc/build/shrinksrc` plus `DC_AUDIO=1` ships an 8-track sequencer and 8 mixer
channels with no diagnostic at all.
"""

import argparse
import json
import os
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

    # -- S1c: aNPC_{n,s,k,e}_overlay -----------------------------------------
    # NOT HERE.  src/actor/npc/ac_npc_ctrl.c_inc is built by _s1c_rules() below
    # because S11 (R2) rides in the same entry and needs the --npctex-pool flag,
    # and this tool refuses two rule entries for one path.  Same arrangement as
    # S10 riding in _s7_rules()'s src/game/m_field_make.c entry.

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

    # -- S6: s_assets[] name strings (kb/levers.md P6) ----------------------
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
    # kb/levers.md L3 and kb/levers.md P6 both claim -821,569 B.  That is
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
         " * DC_SRC_SHRINK S6 (kb/levers.md P6): the `path` field and its\n"
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
# S7 — the data_bgd collision split (kb/levers.md P7).
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
# kb/levers.md L3 and kb/levers.md P7 both claimed -236,544 and called it
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
 * DC_SRC_SHRINK S7 (kb/levers.md P7): mFM_bg_data_c::collision[16][16]
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


# ---------------------------------------------------------------------------
# S10 — the acre ground textures, off the disc (R1).
# ---------------------------------------------------------------------------
# Rides in the S7 entry for src/game/m_field_make.c rather than getting one of
# its own: make_src_shrink refuses to write two rule entries for the same path
# (one output file per TU, so the second write would silently drop the first).
#
# WHAT IT REPLACES. mFM_LoadBGCommonTex()'s copy loop (m_field_make.c:1126-1130)
# is the ONLY consumer, anywhere in src/ / include/ / pc/, of the 96 mFM_grd_*
# source arrays. It runs at three call sites — :1221 (mFM_FieldInit,
# update_tex=TRUE, 27 slots) and :1745 / :1754 (mFM_toSummer /
# mFM_returnSeason, update_tex=FALSE, 21 slots, reached from the island boat
# trip at ac_boat_demo_move.c_inc:92-102) — so the replacement has to be safe
# during gameplay, not only at load. dc/src/dc_bgtex.c is written for that.
#
# WHY THE POINTER IS ENOUGH. make_stub_data.py rewrites an unkept asset to
# `u8 x[1];` — one byte of .bss with a unique address — and the six segment
# tables still name those symbols, so bg_tex_tbl[i] identifies which source the
# slot wants. No season logic and no table duplication in dc/.
#
# ⚠️ THE STALE-TREE GUARD IS NOT OPTIONAL, and it points BOTH ways.
# dc/build/shrinksrc and dc/build/stubsrc are generated host-side and survive a
# flag flip; flags.stamp forces a rebuild but cannot regenerate either tree. A
# demand-ON m_field_make.c compiled against a demand-OFF map is the bad case:
# the map is empty, every load falls through dc_bgtex_load()'s memmove out of a
# [1]-sized array, and the ground renders as garbage rather than failing. The
# reverse (demand-OFF tree, demand-ON build) merely does nothing, and is guarded
# too so it cannot be mistaken for "R1 did not help".
#
# `defined(DC_BGTEX_DEMAND) && …`, not a bare test: the undefined case must stay
# quiet so a tree built by hand without the Makefile's -D still compiles.
def BGTEX_GUARD(demand):
    if demand:
        return (
            "/* " + "-" * 68 + "\n"
            " * DC_SRC_SHRINK S10 — THIS COPY WAS GENERATED FOR"
            " DC_BGTEX_DEMAND=1.\n"
            " * The loop below reads its source arrays off the disc, and it can\n"
            " * only do that because tools/dcstub/make_stub_data.py emitted\n"
            " * dc/build/stubsrc/dc_bgtex_map.inc in the same pass. With the\n"
            " * switch off that map is EMPTY, every load falls back to a memmove\n"
            " * out of a [1]-sized source array, and the town ground is garbage\n"
            " * rather than loudly missing. Refuse to build.\n"
            " * " + "-" * 68 + " */\n"
            "#if defined(DC_BGTEX_DEMAND) && !DC_BGTEX_DEMAND\n"
            "#error \"DC_SRC_SHRINK S10: dc/build/shrinksrc was generated with "
            "--bgtex-demand=1, but this build defines DC_BGTEX_DEMAND=0. Re-run "
            "tools/dcstub/make_src_shrink.py and tools/dcstub/make_stub_data.py "
            "with --bgtex-demand=0 (or rm -rf dc/build/shrinksrc "
            "dc/build/stubsrc) before building.\"\n"
            "#endif")
    return (
        "/* " + "-" * 68 + "\n"
        " * DC_SRC_SHRINK S10 — THIS COPY WAS GENERATED FOR DC_BGTEX_DEMAND=0.\n"
        " * The copy loop below is the vendored bcopy, so the mFM_grd_* source\n"
        " * arrays must be RESIDENT — which is why make_stub_data.py puts the 27\n"
        " * summer/shared files back on the keep list at the same setting. A\n"
        " * DC_BGTEX_DEMAND=1 build against this tree would silently keep 80,736\n"
        " * B of .bss and read as \"R1 saved nothing\".\n"
        " * " + "-" * 68 + " */\n"
        "#if defined(DC_BGTEX_DEMAND) && DC_BGTEX_DEMAND\n"
        "#error \"DC_SRC_SHRINK S10: dc/build/shrinksrc was generated with "
        "--bgtex-demand=0, but this build defines DC_BGTEX_DEMAND=1. Re-run "
        "tools/dcstub/make_src_shrink.py and tools/dcstub/make_stub_data.py "
        "with --bgtex-demand=1 (or rm -rf dc/build/shrinksrc dc/build/stubsrc) "
        "before building.\"\n"
        "#endif")


def _s10_rules(demand):
    """The R1 rules that ride in S7's src/game/m_field_make.c entry."""
    head = BGTEX_GUARD(demand) + "\n"
    if demand:
        head += (
            "/* DC_SRC_SHRINK S10: bg_tex_tbl[i] is the SOURCE ARRAY'S ADDRESS,\n"
            " * and under DC_ASSET_STUB that array is one byte long. dc_bgtex.c\n"
            " * uses the address as a key into the generated pointer -> ROM map\n"
            " * and reads the bytes off the disc straight into the staging\n"
            " * buffer. Declared here rather than pulled from dc_platform.h: this\n"
            " * TU is vendored decomp and must not grow a dc/ include. */\n"
            "extern void dc_bgtex_load(const void* src, void* dest,\n"
            "                          unsigned int size);\n")

    rules = [
        (1,
         r"^static void mFM_LoadBGCommonTex\(int update_tex, u8 tex_idx\) \{$",
         head + "static void mFM_LoadBGCommonTex(int update_tex, u8 tex_idx) {"),
    ]
    if demand:
        rules.append((
            1,
            r"^            bcopy\(bg_tex_tbl\[i\], l_bg_tex_common_dummy\[i\]\.data,"
            r" l_bg_tex_common_dummy\[i\]\.size\);$",
            # The DEST size, not the s_assets[] size — l_bg_tex_common_dummy[15]
            # asks for 0x800 out of a 0x400 mFM_grd_s_beach_tex, in all six
            # tables, and reading 0x800 from rom_off is what reproduces the
            # GameCube's own over-read. dc_bgtex.c logs the discrepancy.
            "            dc_bgtex_load(bg_tex_tbl[i],"
            " l_bg_tex_common_dummy[i].data,\n"
            "                          l_bg_tex_common_dummy[i].size); " + MARK))
    return rules


# ===========================================================================
# S11 (R2) — the villager texture pool's two seams.
# ===========================================================================
# WHAT IT REPLACES.  Nothing: S11 inserts two calls and deletes no code.
#
#   LOAD    aNPC_dma_draw_data_proc (ac_npc_ctrl.c_inc:688) gains one statement
#           before its mem_copy.  That is the ONE choke point every villager
#           texture snapshot goes through — ac_npc_ct.c_inc:255 calls it
#           directly and m_actor.c:135 reaches it through
#           Common_Get(clip).npc_clip->dma_draw_data_proc — so a pool load
#           placed there is correct by construction.
#   RESET   the two mNpc_SetNpcList() call sites that rebuild the WHOLE
#           npclist: m_start_data_init.c:559 (mSDI_StartInitAfter, i.e.
#           player-select / game start) and m_trademark.c:76
#           (set_npc_4_title_demo, before its GAME_GOTO_NEXT into PLAY).
#
# ⚠️ WHY THE RESET CANNOT GO ANYWHERE ELSE.  The pool never evicts, because an
# actor takes a PRIVATE COPY of the 16 texture pointers at construct time
# (ac_npc_ct.c_inc:256) and reusing a slot under a live actor puts another
# animal's skin on it silently.  The two sites above are the only points in the
# tree that rebuild the npclist AND are provably between scene teardowns, i.e.
# where no NPC actor can exist.  The other two mNpc_SetNpcList() callers —
# ac_boat_demo_move.c_inc:29 and ac_npc_ctrl.c_inc:878, both the islander, both
# count 1 — run mid-play and are deliberately left alone; the islander just
# takes the 16th slot.
#
# WHY NOT --wrap=mNpc_SetNpcList.  It would be a silent no-op.  sh-elf uses a
# leading-underscore user label prefix — `sh-elf-nm` on the linked image gives
# `T _mNpc_SetNpcList` and `T _main` (dc/build/dedup/syms.txt) — so ld would
# need `--wrap=_mNpc_SetNpcList` against an asm name of `__wrap__mNpc_SetNpcList`,
# and --wrap on a symbol that does not exist is ignored without a diagnostic.
# A rewrite here is anchored, so drift is a hard error instead.
#
# WHY THE ASSERT BLOCK IS NOT OPTIONAL.  dc/src/dc_npctex.c reaches
# npc_draw_data_tbl[] as raw bytes — it must not include ac_npc.h — so the row
# stride and the four member offsets are literals over there.  This TU has the
# header, so it is where they get pinned.  Every constant dc_npctex.c hardcodes
# is in the assert.
NPCTEX_LAYOUT_ASSERT = (
    "sizeof(aNPC_draw_data_c) == 0x6C && "
    "sizeof(aNPC_draw_tex_data_c) == 0x4C && "
    "__builtin_offsetof(aNPC_draw_data_c, tex_data) == 0x08 && "
    "__builtin_offsetof(aNPC_draw_tex_data_c, texture) == 0x00 && "
    "__builtin_offsetof(aNPC_draw_tex_data_c, palette) == 0x04 && "
    "__builtin_offsetof(aNPC_draw_tex_data_c, eye_texture) == 0x08 && "
    "__builtin_offsetof(aNPC_draw_tex_data_c, mouth_texture) == 0x28 && "
    "aNPC_EYE_TEX_NUM == 8 && aNPC_MOUTH_TEX_NUM == 6 && "
    "ALL_NPC_NUM == 238 && ANIMAL_NUM_MAX == 15"
)


def NPCTEX_GUARD(pool):
    """The stale-tree guard, pointing both ways — same argument as R1's."""
    if pool:
        return (
            "/* " + "-" * 68 + "\n"
            " * DC_SRC_SHRINK S11 — THIS COPY WAS GENERATED FOR"
            " DC_NPCTEX_POOL=1.\n"
            " * The pool calls below can only do anything because\n"
            " * tools/dcstub/make_stub_data.py emitted\n"
            " * dc/build/stubsrc/dc_npctex_map.inc in the same pass. With the\n"
            " * switch off that map is EMPTY, dc_npctex_ensure() is a no-op AND\n"
            " * the 21 censused villager tex files are back on the keep list —\n"
            " * so this tree would pay 90,464 B of .bss for textures the pool\n"
            " * thinks it is serving. Refuse to build.\n"
            " * " + "-" * 68 + " */\n"
            "#if defined(DC_NPCTEX_POOL) && !DC_NPCTEX_POOL\n"
            "#error \"DC_SRC_SHRINK S11: dc/build/shrinksrc was generated with "
            "--npctex-pool=1, but this build defines DC_NPCTEX_POOL=0. Re-run "
            "tools/dcstub/make_src_shrink.py and tools/dcstub/make_stub_data.py "
            "with --npctex-pool=0 (or rm -rf dc/build/shrinksrc "
            "dc/build/stubsrc) before building.\"\n"
            "#endif")
    return (
        "/* " + "-" * 68 + "\n"
        " * DC_SRC_SHRINK S11 — THIS COPY WAS GENERATED FOR DC_NPCTEX_POOL=0.\n"
        " * No pool call is inserted, so the villager texture arrays must be\n"
        " * RESIDENT — which is why make_stub_data.py puts the 21 censused\n"
        " * villager tex files back on the keep list at the same setting. A\n"
        " * DC_NPCTEX_POOL=1 build against this tree would emit the map, keep\n"
        " * nothing, call nothing, and render every villager untextured — which\n"
        " * looks exactly like a missing asset (kb/traps.md).\n"
        " * " + "-" * 68 + " */\n"
        "#if defined(DC_NPCTEX_POOL) && DC_NPCTEX_POOL\n"
        "#error \"DC_SRC_SHRINK S11: dc/build/shrinksrc was generated with "
        "--npctex-pool=0, but this build defines DC_NPCTEX_POOL=1. Re-run "
        "tools/dcstub/make_src_shrink.py and tools/dcstub/make_stub_data.py "
        "with --npctex-pool=1 (or rm -rf dc/build/shrinksrc dc/build/stubsrc) "
        "before building.\"\n"
        "#endif")


# ===========================================================================
# S12 (R3) — the villager MODEL pool, riding S11's three seams.
# ===========================================================================
# WHAT IT REPLACES.  Nothing: S12 inserts two more calls beside S11's and
# deletes no code.  Same choke point, same two reset sites, same argument for
# both — see the S11 block above, which is the one that reasons about why those
# three places and no others.
#
# WHAT IS DIFFERENT.  R2 moved textures, which the draw binds through segment
# registers; R3 moves VERTEX ARRAYS, which 933 display-list words point at by a
# LINK-TIME-BAKED ADDRESS.  So dc_npcmdl_ensure() does not just repoint a struct
# field, it rewrites word 1 of every gsSPVertex Gfx of that species.  All of the
# reasoning about which words, and why they are found from a generated table
# rather than by walking the list, is in dc/src/dc_npcmdl.c.
#
# ⚠️ THE SEAM IS LESS LOAD-BEARING HERE, AND THE RESET IS MORE SO.  A villager's
# texture pointers are snapshotted per actor (ac_npc_ct.c_inc:256), so R2 had to
# patch before the copy.  Display lists are GLOBAL and shared by every actor of
# the species, so R3 could in principle patch later — but it must never patch
# a species OUT from under a live actor, which is why it takes the same two
# provably actor-free reset points and never evicts.
#
# WHY THE ASSERT BLOCK IS NOT OPTIONAL.  dc/src/dc_npcmdl.c walks
# npc_draw_data_tbl[] -> cKF_Skeleton_R_c -> cKF_Joint_R_c -> Gfx entirely in
# raw bytes, because it cannot include ac_npc.h or c_keyframe.h.  Every literal
# it hardcodes is pinned here, in the TU that does have the headers — including
# sizeof(Vtx), which is what turns a generated element index into a byte offset,
# and G_VTX, which the runtime probe checks each word against.
NPCMDL_LAYOUT_ASSERT = (
    "__builtin_offsetof(aNPC_draw_data_c, model_skeleton) == 0x04 && "
    "sizeof(cKF_Skeleton_R_c) == 8 && "
    "__builtin_offsetof(cKF_Skeleton_R_c, num_joints) == 0 && "
    "__builtin_offsetof(cKF_Skeleton_R_c, joint_table) == 4 && "
    "sizeof(cKF_Joint_R_c) == 12 && "
    "__builtin_offsetof(cKF_Joint_R_c, model) == 0 && "
    "sizeof(Gfx) == 8 && sizeof(Vtx) == 16 && G_VTX == 1"
)


def NPCMDL_GUARD(pool):
    """The stale-tree guard, pointing both ways — same argument as R1's."""
    if pool:
        return (
            "/* " + "-" * 68 + "\n"
            " * DC_SRC_SHRINK S12 — THIS COPY WAS GENERATED FOR"
            " DC_NPCMDL_POOL=1.\n"
            " * The pool calls below can only do anything because\n"
            " * tools/dcstub/make_stub_data.py emitted\n"
            " * dc/build/stubsrc/dc_npcmdl_map.inc in the same pass. With the\n"
            " * switch off that map is EMPTY, dc_npcmdl_ensure() is a no-op AND\n"
            " * cbr_1 is back on the keep list — so this tree would pay 5,536 B\n"
            " * of .bss for a model the pool thinks it is serving, and the\n"
            " * other 31 villager species would silently stay stubbed.\n"
            " * Refuse to build.\n"
            " * " + "-" * 68 + " */\n"
            "#if defined(DC_NPCMDL_POOL) && !DC_NPCMDL_POOL\n"
            "#error \"DC_SRC_SHRINK S12: dc/build/shrinksrc was generated with "
            "--npcmdl-pool=1, but this build defines DC_NPCMDL_POOL=0. Re-run "
            "tools/dcstub/make_src_shrink.py and tools/dcstub/make_stub_data.py "
            "with --npcmdl-pool=0 (or rm -rf dc/build/shrinksrc "
            "dc/build/stubsrc) before building.\"\n"
            "#endif")
    return (
        "/* " + "-" * 68 + "\n"
        " * DC_SRC_SHRINK S12 — THIS COPY WAS GENERATED FOR DC_NPCMDL_POOL=0.\n"
        " * No pool call is inserted, so a villager's vertex array must be\n"
        " * RESIDENT — which is why make_stub_data.py puts cbr_1 back on the\n"
        " * keep list at the same setting. A DC_NPCMDL_POOL=1 build against\n"
        " * this tree would emit the map, keep nothing, call nothing, and\n"
        " * every villager would draw as a black spiky mess — which looks\n"
        " * exactly like a missing asset (kb/traps.md).\n"
        " * " + "-" * 68 + " */\n"
        "#if defined(DC_NPCMDL_POOL) && DC_NPCMDL_POOL\n"
        "#error \"DC_SRC_SHRINK S12: dc/build/shrinksrc was generated with "
        "--npcmdl-pool=0, but this build defines DC_NPCMDL_POOL=1. Re-run "
        "tools/dcstub/make_src_shrink.py and tools/dcstub/make_stub_data.py "
        "with --npcmdl-pool=1 (or rm -rf dc/build/shrinksrc dc/build/stubsrc) "
        "before building.\"\n"
        "#endif")


def _s1c_rules(npctex_pool, npcmdl_pool, npcdiag=0):
    """src/actor/npc/ac_npc_ctrl.c_inc — S1c's four arenas, the setupActor
    return fix, S11's + S12's load seam, and N3's two diagnostic seams. One
    entry, because this tool writes one scratch copy per TU.

    SHADOW.  ac_npc_ctrl.c_inc is #included as
    "../src/actor/npc/ac_npc_ctrl.c_inc" from src/actor/npc/ac_npc.c:130 AND
    from src/actor/npc/ac_npc2.c:152 (an earlier revision of this comment said
    "exactly once" — it is twice, and both TUs want the rewrite). That relative
    path cannot resolve against the includer's own directory, so it falls
    through to -I and lands in the shrink tree. VERIFIED with -H:
      . /…/shrinksrc/include/../src/actor/npc/ac_npc_ctrl.c_inc
    Both the struct definitions and the arrays live in this one file, so the
    shadow is self-contained and cannot half-apply.

    aNPC_actor_class_overlay_c::buf[0x9D0] is deliberately NOT touched: it is
    REAL storage — aNPC_keep_actor_class() hands its address out as a
    NPC_ACTOR* (ac_npc_ctrl.c_inc:733) and aNPC_get_actor_area_proc returns it
    for allocations up to 0x9D0 B.
    """
    rules = [
        (1, r"^    u8 buf\[0x800\];$",  "    u8 buf[0x10]; " + MARK),
        (1, r"^    u8 buf\[0x2000\];$", "    u8 buf[0x10]; " + MARK),
        (1, r"^    u8 buf\[0x3000\];$", "    u8 buf[0x10]; " + MARK),
        (1, r"^    u8 buf\[0x2800\];$", "    u8 buf[0x10]; " + MARK),

        # -- CORRECTNESS, not size. The one exception in this file. ----------
        # aNPC_setupActor_proc is `static int` and FALLS OFF ITS END: its last
        # statement is an unused call to aNPC_setupActor_sub, which does return
        # an int. Reading the result of a function that returned nothing is
        # undefined, and four call sites read it -- through the
        # npc_clip->setupActor_proc pointer this function is installed in at
        # :818 and :1075:
        #
        #   m_post_office.c:399,416   spawned_postman  (the postman)
        #   ac_groundhog_control.c:134 spawned_actor   (the shrine groundhog)
        #   ac_shrine_move.c_inc:317   made_hem
        #   ac_birth_control.c:213     was_born        (VILLAGER BIRTH)
        #
        # It happens to work at -O0 on SH-4 because aNPC_setupActor_sub's
        # return value is already in r0 and this epilogue does not clobber it.
        # That is an accident of codegen, not a guarantee, and it is exactly
        # the class of bug that appears when something unrelated moves.
        # Upstream ACGC-PC-Port hit it as a hard softlock and fixed it in
        # 3b650ae1 ("Fix Nook not spawning softlock"); this is that commit's
        # first hunk, and the ONLY part of it that applies to us -- the rest of
        # 3b650ae1 enlarges aNPC_actor_class_overlay_c::buf from 0x9D0 to 0xA40
        # for a 64-bit ABI, and sizeof(NPC_GUIDE_ACTOR) is exactly 0x9D0 here.
        #
        # ⚠️ COUPLING: this rides the DC_SRC_SHRINK tree, so DC_SRC_SHRINK=0
        # turns the fix off along with the shrinks. That is acceptable only
        # because the -O0 codegen accident above makes the unfixed form work
        # today; if this ever becomes load-bearing, move it to its own
        # rewriter with its own switch.
        (1,
         r"^    aNPC_setupActor_sub\(play, idx, name, profile, &pos, mvlist_no, arg\);$",
         "    return aNPC_setupActor_sub(play, idx, name, profile, &pos, mvlist_no, arg); "
         + MARK),
    ]

    # -- S11 (R2): the load seam. See the block comment above NPCTEX_GUARD. --
    # The WHOLE function is the anchor, not just the mem_copy line: the
    # replacement has to introduce the extern and the layout assert at file
    # scope, and anchoring on the body is what proves `idx` is the value the
    # mem_copy is about to index with.
    body = (
        r"^static void aNPC_dma_draw_data_proc\(aNPC_draw_data_c\* draw_data_p,"
        r" mActor_name_t npc_name\) \{\n"
        r"    int idx = aNPC_get_draw_data_idx\(npc_name\);\n"
        r"\n"
        r"    mem_copy\(\(u8\*\)draw_data_p, \(u8\*\)&npc_draw_data_tbl\[idx\],"
        r" sizeof\(aNPC_draw_data_c\)\);\n"
        r"\}$"
    )
    head = NPCTEX_GUARD(npctex_pool) + "\n"
    if npctex_pool:
        head += (
            "/* DC_SRC_SHRINK S11: R2's LOAD seam. `idx` is the\n"
            " * npc_draw_data_tbl[] row this function is about to snapshot, and\n"
            " * the snapshot is what the DRAW reads (ac_npc_draw.c_inc:227 uses\n"
            " * the actor's private &nactorx->draw.draw_tex_data, not the\n"
            " * table) — so the pool has to fill the row HERE, one statement\n"
            " * earlier, or a later patch changes nothing that is ever drawn.\n"
            " * dc_npctex_ensure() ignores idx < 0 (this function does not) and\n"
            " * every special-NPC row, so Tom Nook and the raccoons are\n"
            " * untouched. Declared rather than pulled from dc_platform.h: this\n"
            " * TU is vendored decomp and must not grow a dc/ include. */\n"
            "extern void dc_npctex_ensure(int row);\n"
            "/* dc/src/dc_npctex.c hardcodes this layout in raw bytes because it\n"
            " * cannot include ac_npc.h. This is where those literals are\n"
            " * pinned; if the vendored struct moves, this TU stops compiling\n"
            " * instead of the pool writing pointers into the wrong fields. */\n"
            + ASSERT("npctex_layout", NPCTEX_LAYOUT_ASSERT) + "\n")
    head += NPCMDL_GUARD(npcmdl_pool) + "\n"
    if npcmdl_pool:
        head += (
            "/* DC_SRC_SHRINK S12: R3's LOAD seam, riding S11's. Same `idx`,\n"
            " * same choke point — the two halves of a villager have to arrive\n"
            " * together or R2 paints a correct skin onto a black spiky mess.\n"
            " * dc_npcmdl_ensure() ignores idx < 0 and every special-NPC row,\n"
            " * and the 40 special skeletons are not in its map at all, so Tom\n"
            " * Nook and the raccoons are untouched twice over. */\n"
            "extern void dc_npcmdl_ensure(int row);\n"
            "/* dc/src/dc_npcmdl.c walks npc_draw_data_tbl[] -> the skeleton ->\n"
            " * the joint table -> a Gfx word in raw bytes, because it cannot\n"
            " * include ac_npc.h or c_keyframe.h. This is where those literals\n"
            " * are pinned; if the vendored structs move, this TU stops\n"
            " * compiling instead of the pool writing a pool address into some\n"
            " * other display-list word. */\n"
            + ASSERT("npcmdl_layout", NPCMDL_LAYOUT_ASSERT) + "\n")

    tail = (
        "static void aNPC_dma_draw_data_proc(aNPC_draw_data_c* draw_data_p,"
        " mActor_name_t npc_name) {\n"
        "    int idx = aNPC_get_draw_data_idx(npc_name);\n"
        "\n"
    )
    if npctex_pool:
        tail += "    dc_npctex_ensure(idx); " + MARK + "\n"
    if npcmdl_pool:
        tail += "    dc_npcmdl_ensure(idx); " + MARK + "\n"
    tail += (
        "    mem_copy((u8*)draw_data_p, (u8*)&npc_draw_data_tbl[idx],"
        " sizeof(aNPC_draw_data_c));\n"
        "}"
    )
    rules.append((1, body, head + tail))

    # -- N3: the villager-construction diagnostic --------------------------
    # Appended LAST so none of the rules above can be disturbed by it, and
    # UNCONDITIONALLY — at --npcdiag=0 it contributes only the stale-tree
    # guard, which is the whole reason it is not simply omitted. See
    # _npcdiag_ctrl_rules() for the argument.
    rules += _npcdiag_ctrl_rules(npcdiag)

    return ("src/actor/npc/ac_npc_ctrl.c_inc", "shadow", rules)


# ===========================================================================
# N3 — the villager-construction diagnostic (dc/src/dc_npcdiag.c).
# ===========================================================================
# WHAT IT REPLACES.  Nothing.  N3 inserts calls and deletes no code; every
# rewritten predicate is wrapped in `dc_npcdiag_gate(SLOT, <predicate>)`, which
# returns its second argument untouched, so not one branch changes shape and the
# short-circuit order of every `&&` is preserved exactly.
#
# WHY IT IS ONE INSTRUMENT AND NOT FIVE.  The question — "why is no villager
# actor ever constructed" — has five live answers and they live in four
# different files, five functions apart.  Testing them one at a time is five
# burns; every gate on the chain is counted here so ONE Flycast town run is
# decisive.  dc/src/dc_npcdiag.c's header enumerates the five and names the
# field on the printed line that kills each.
#
# THE THREE TUs, AND WHY THOSE THREE.
#
#   src/actor/ac_set_npc_manager.c   SWAP.  The manager: the wade dispatch, the
#       REGULAR and GUEST passes with their five serial gates each, the make
#       list, and aSNMgr_check_safe_ut (the inside of the utnum gate).  A .c
#       compiled directly, so -I can never reach it; not in stub.list.
#   src/actor/npc/ac_npc_cloth.c_inc SHADOW.  aNPC_keep_cloth_data_area, the one
#       place the ten cloth banks are reserved.  Included as
#       "../src/actor/npc/ac_npc_cloth.c_inc" from BOTH src/actor/npc/ac_npc.c
#       :114 and src/actor/npc/ac_npc2.c:136 — the same relative form as
#       ac_npc_ctrl.c_inc, which cannot resolve against the includer's own
#       directory and therefore falls through to -I and lands here.
#   src/actor/npc/ac_npc_ctrl.c_inc  SHADOW, and it RIDES S1c's entry rather
#       than getting one of its own: main() refuses two rule entries for one
#       path.  aNPC_setupActor_sub's two gates and aNPC_actor_ct_c's clip
#       install.
#
# ⚠️ THE STALE-TREE GUARD IS EMITTED AT --npcdiag=0 TOO, and that is the one
# place N3 differs from R1/R2/R3.  Their failure mode is a wrong image; N3's is
# worse, because every counter reading ZERO IS A MEANINGFUL RESULT.  A tree
# generated with --npcdiag=0 built with -DDC_NPCDIAG=1 would print a full
# [DC/NPCDIAG] line with every field at zero and nothing to say it was lying.
# So the guard goes into ac_npc_ctrl.c_inc — the one N3 TU that is in the tree
# at every setting — and it points BOTH ways.  The other two entries only exist
# at 1, which is what keeps --npcdiag=0 a byte-identical revert: no extra file
# in shrink.list, no extra shadow, nothing.
#
# ⚠️ THE SLOT NUMBERS ARE PARSED OUT OF dc/include/dc_npcdiag.h, not restated.
# Two copies of an enum whose members are only ever compared by NUMBER is a
# silent-relabelling bug waiting to happen: the build would succeed and the
# counter named `scope=` would be reporting `appear=`.  _npcdiag_slots() reads
# the header, requires every name this file emits, and hard-errors otherwise.
NPCDIAG_HDR = "dc/include/dc_npcdiag.h"

# Every slot this generator emits. Checked against the header, so adding a call
# site without adding its enum row is a generator error rather than an `oob=`
# nobody reads.
NPCDIAG_REQUIRED = (
    "DC_NPCDIAG_G_REG_CALL", "DC_NPCDIAG_G_REG_EXIST", "DC_NPCDIAG_G_REG_SCOPE",
    "DC_NPCDIAG_G_REG_APPEAR", "DC_NPCDIAG_G_REG_UTNUM", "DC_NPCDIAG_G_REG_MAKE",
    "DC_NPCDIAG_G_GST_CALL", "DC_NPCDIAG_G_GST_ARBEIT", "DC_NPCDIAG_G_GST_BLKMAX",
    "DC_NPCDIAG_G_GST_EXIST", "DC_NPCDIAG_G_GST_SCOPE", "DC_NPCDIAG_G_GST_APPEAR",
    "DC_NPCDIAG_G_GST_UTNUM", "DC_NPCDIAG_G_GST_MAKE",
    "DC_NPCDIAG_G_MK_ENT", "DC_NPCDIAG_G_MK_GATE", "DC_NPCDIAG_G_MK_SLOT",
    "DC_NPCDIAG_G_MK_IDX", "DC_NPCDIAG_G_MK_CALLED", "DC_NPCDIAG_G_MK_RET",
    "DC_NPCDIAG_G_SU_ENT", "DC_NPCDIAG_G_SU_CHK", "DC_NPCDIAG_G_SU_ACTOR",
    "DC_NPCDIAG_G_UT_CALL", "DC_NPCDIAG_G_UT_COL", "DC_NPCDIAG_G_UT_FGCOL",
    "DC_NPCDIAG_G_UT_HGAP",
)

_NPCDIAG_SLOT_CACHE = {}


def _npcdiag_slots():
    """Parse the gate enum out of dc/include/dc_npcdiag.h.

    Returns an ordered list of (name, value). The header is the single source
    of truth; this refuses to guess. Cached because three rule builders ask.
    """
    if _NPCDIAG_SLOT_CACHE:
        return _NPCDIAG_SLOT_CACHE["slots"]

    hdr = REPO / NPCDIAG_HDR
    if not hdr.exists():
        raise SystemExit(
            "make_src_shrink N3: %s is missing. It is the single source of\n"
            "  truth for the gate slot numbers this generator emits into the\n"
            "  vendored TUs; without it the rewrite would have to restate them\n"
            "  and could relabel a counter silently." % NPCDIAG_HDR)

    text = hdr.read_text(encoding="utf-8", errors="surrogateescape")
    m = re.search(r"^enum \{$(.*?)^\};$", text, re.MULTILINE | re.DOTALL)
    if m is None or "DC_NPCDIAG_G_NUM" not in m.group(1):
        raise SystemExit(
            "make_src_shrink N3: could not find the `enum { ... DC_NPCDIAG_G_NUM"
            " ... };`\n  block in %s. Re-derive this parser against the header "
            "rather than\n  hardcoding the slot numbers here." % NPCDIAG_HDR)

    names = re.findall(r"^\s*(DC_NPCDIAG_G_[A-Z0-9_]+)", m.group(1), re.MULTILINE)
    if not names or names[-1] != "DC_NPCDIAG_G_NUM":
        raise SystemExit(
            "make_src_shrink N3: the gate enum in %s must END with "
            "DC_NPCDIAG_G_NUM;\n  got %r." % (NPCDIAG_HDR, names[-1:] or None))
    # The first row must pin itself to 0 explicitly. Without that the C enum
    # would still start at 0, but the header would no longer SAY so, and this
    # parser's contract is that it reads what the header states.
    if not re.search(r"^\s*%s\s*=\s*0\s*," % names[0], m.group(1), re.MULTILINE):
        raise SystemExit(
            "make_src_shrink N3: the first gate row in %s must be spelled "
            "`%s = 0,`."
            % (NPCDIAG_HDR, names[0]))

    slots = [(n, i) for i, n in enumerate(names[:-1])]
    have = {n for n, _ in slots}
    missing = [n for n in NPCDIAG_REQUIRED if n not in have]
    if missing:
        raise SystemExit(
            "make_src_shrink N3: %s does not declare %s.\n"
            "  This generator emits those slots into the vendored TUs; a call "
            "site with\n  no enum row would land on `oob=` and be counted "
            "nowhere." % (NPCDIAG_HDR, ", ".join(missing)))

    _NPCDIAG_SLOT_CACHE["slots"] = slots
    return slots


def NPCDIAG_GUARD(diag):
    """N3's stale-tree guard, both directions. See the block comment above:
    unlike R1/R2/R3 this one is emitted at 0 as well, because a mismatched tree
    produces a diagnostic line full of zeroes and zero is a RESULT here."""
    if diag:
        return (
            "#if defined(DC_NPCDIAG) && !DC_NPCDIAG\n"
            "#error \"DC_SRC_SHRINK N3: dc/build/shrinksrc was generated with "
            "--npcdiag=1, so the villager-construction gates are instrumented -- "
            "but this build defines DC_NPCDIAG=0, where every dc_npcdiag_*() is "
            "an empty function. Re-run tools/dcstub/make_src_shrink.py with "
            "--npcdiag=0 (or rm -rf dc/build/shrinksrc) before building.\"\n"
            "#endif")
    return (
        "#if defined(DC_NPCDIAG) && DC_NPCDIAG\n"
        "#error \"DC_SRC_SHRINK N3: dc/build/shrinksrc was generated with "
        "--npcdiag=0, so NOTHING calls dc_npcdiag_gate() -- but this build "
        "defines DC_NPCDIAG nonzero. The [DC/NPCDIAG] line would print every "
        "counter as zero, which is a MEANINGFUL READING and would be a lie. "
        "Re-run tools/dcstub/make_src_shrink.py with --npcdiag=1 (or rm -rf "
        "dc/build/shrinksrc) before building.\"\n"
        "#endif")


def NPCDIAG_HEAD():
    """The declarations every N3-rewritten TU needs, emitted before its first
    injected call.

    Self-guarded with DC_NPCDIAG_DECLS_ because ac_npc.c and ac_npc2.c each
    include BOTH ac_npc_cloth.c_inc and ac_npc_ctrl.c_inc, in that order — so
    two copies of this block land in one translation unit.

    Declared here rather than pulled from dc/include/dc_npcdiag.h for the same
    reason S10/S11/S12 declare theirs: these TUs are vendored decomp and must
    not grow a dc/ include (CLAUDE.md §1)."""
    slots = _npcdiag_slots()
    out = [
        "/* " + "-" * 68,
        " * DC_SRC_SHRINK N3: the villager-construction diagnostic",
        " * (dc/src/dc_npcdiag.c). dc_npcdiag_gate() RETURNS ITS SECOND ARGUMENT",
        " * UNTOUCHED, so every wrapped predicate keeps its value, its type and",
        " * its position in the enclosing short-circuit chain -- no branch in",
        " * this file changes shape.",
        " *",
        " * The slot numbers below are GENERATED from " + NPCDIAG_HDR + ",",
        " * which is their single source of truth; do not edit them here.",
        " * " + "-" * 68 + " */",
        "#ifndef DC_NPCDIAG_DECLS_",
        "#define DC_NPCDIAG_DECLS_",
        "extern int  dc_npcdiag_gate(int slot, int val);",
        "extern void dc_npcdiag_tick(int wade, int set_mode, int next_bx,",
        "                            int next_bz, int now_bx, int now_bz,",
        "                            unsigned int exist, unsigned int appear,",
        "                            const void* npclist);",
        "extern void dc_npcdiag_mgr_ct(void);",
        "extern void dc_npcdiag_ctrl_ct(int is_npc2, int clip_was_already_set);",
        "extern void dc_npcdiag_clip_set(int is_npc2);",
        "extern void dc_npcdiag_cloth_begin(void);",
        "extern void dc_npcdiag_cloth_bank(int ok);",
        "/* Which NPC controller this TU is being compiled as. ac_npc2.c defines",
        " * aNPC_NPC2 before it includes either .c_inc, and only the non-NPC2",
        " * flavour installs CLIP(npc_clip)->setupActor_proc",
        " * (ac_npc_ctrl.c_inc:817-821) -- so a clip installed by the NPC2",
        " * controller is non-NULL and STILL fails aSNMgr_make_npc's gate. That",
        " * is exactly the shape hypothesis 3 predicts, and it is why the",
        " * one-shot reports which controller it came from. */",
        "#ifdef aNPC_NPC2",
        "#define DC_NPCDIAG_IS_NPC2 1",
        "#else",
        "#define DC_NPCDIAG_IS_NPC2 0",
        "#endif",
    ]
    out += ["#define %-28s %d" % (n, v) for n, v in slots]
    out.append("#endif /* DC_NPCDIAG_DECLS_ */")
    return "\n".join(out) + "\n"


def _lit(text):
    """An anchor for a verbatim multi-line block.

    re.escape rather than hand-written escaping: these blocks are 5-15 lines of
    C with parentheses, brackets, `*`, `|` and `+` in them, and one missed
    backslash is a rule that silently stops matching -- the exact failure this
    whole file is built around. `^`/`$` are MULTILINE-anchored by apply_rules,
    so the block must start at a line start and end at a line end."""
    return "^" + re.escape(text) + "$"


# --- N3's three rule builders ---------------------------------------------
# Each is anchored on a WHOLE function or a whole contiguous gate chain, never
# on a single line, so a mis-anchor is impossible rather than merely unlikely
# (the standard set by the block comment above NPCTEX_GUARD).

def _npcdiag_mgr_rules():
    """src/actor/ac_set_npc_manager.c — SWAP. Only built at --npcdiag=1."""
    return ("src/actor/ac_set_npc_manager.c", "swap", [
        # 1. The declarations, after the last include and before every use.
        (1, _lit('#include "m_event_map_npc.h"'),
         '#include "m_event_map_npc.h"\n\n' + NPCDIAG_HEAD()),

        # 2. aSNMgr_actor_ct's tail. Anchored through the closing brace because
        #    this setup_set_proc(REGULAR) line also appears in
        #    aSNMgr_actor_move's mFI_WADE_START arm, where it is followed by
        #    `break;` instead. Without this counter, every zero below is
        #    ambiguous: "the gate failed" and "the manager was never
        #    constructed" would look identical.
        (1, _lit("    aSNMgr_setup_set_proc(manager, aSNMgr_SET_MODE_REGULAR);\n"
                 "}"),
         "    aSNMgr_setup_set_proc(manager, aSNMgr_SET_MODE_REGULAR);\n"
         "    dc_npcdiag_mgr_ct(); " + MARK + "\n"
         "}"),

        # 3. aSNMgr_check_safe_ut — the INSIDE of the utnum gate, split three
        #    ways so "the field data says no NPC may stand here"
        #    (mNpc_CheckNpcSet_fgcol) is distinguishable from "the ground steps"
        #    (mCoBG_ExistHeightGap_KeepAndNow). Hypothesis 4 lives here.
        (1, _lit(
            "static int aSNMgr_check_safe_ut(int bx, int bz, int ux, int uz, mActor_name_t item, mCoBG_Collision_u* col_p) {\n"
            "    int ret = FALSE;\n"
            "\n"
            "    if (col_p != NULL) {\n"
            "        xyz_t pos;\n"
            "\n"
            "        if (mNpc_CheckNpcSet_fgcol(item, col_p->data.unit_attribute)) {\n"
            "            mFI_BkandUtNum2Wpos(&pos, bx, bz, ux, uz);\n"
            "\n"
            "            if (!mCoBG_ExistHeightGap_KeepAndNow(pos)) {"),
         "static int aSNMgr_check_safe_ut(int bx, int bz, int ux, int uz, mActor_name_t item, mCoBG_Collision_u* col_p) {\n"
         "    int ret = FALSE;\n"
         "\n"
         "    dc_npcdiag_gate(DC_NPCDIAG_G_UT_CALL, 1); " + MARK + "\n"
         "    if (dc_npcdiag_gate(DC_NPCDIAG_G_UT_COL, col_p != NULL)) {\n"
         "        xyz_t pos;\n"
         "\n"
         "        if (dc_npcdiag_gate(DC_NPCDIAG_G_UT_FGCOL, mNpc_CheckNpcSet_fgcol(item, col_p->data.unit_attribute))) {\n"
         "            mFI_BkandUtNum2Wpos(&pos, bx, bz, ux, uz);\n"
         "\n"
         "            if (dc_npcdiag_gate(DC_NPCDIAG_G_UT_HGAP, !mCoBG_ExistHeightGap_KeepAndNow(pos))) {"),

        # 4. aSNMgr_make_npc's head and its CLIP gate. `ent` vs `gate` is what
        #    separates hypothesis 3 (npc_clip NULL / no setupActor_proc) from
        #    "the make list was empty", which look the same from outside.
        (1, _lit(
            "static void aSNMgr_make_npc(SET_NPC_MANAGER_ACTOR* manager, GAME_PLAY* play) {\n"
            "    aSNMgr_make_c* make_p = manager->npc_info.make;\n"
            "    mNpc_NpcList_c* list_p = manager->npc_info.list_p;\n"
            "    int idx;\n"
            "    int make_ret;\n"
            "    int i;\n"
            "\n"
            "    if (CLIP(npc_clip) != NULL && CLIP(npc_clip)->setupActor_proc != NULL) {"),
         "static void aSNMgr_make_npc(SET_NPC_MANAGER_ACTOR* manager, GAME_PLAY* play) {\n"
         "    aSNMgr_make_c* make_p = manager->npc_info.make;\n"
         "    mNpc_NpcList_c* list_p = manager->npc_info.list_p;\n"
         "    int idx;\n"
         "    int make_ret;\n"
         "    int i;\n"
         "\n"
         "    dc_npcdiag_gate(DC_NPCDIAG_G_MK_ENT, 1); " + MARK + "\n"
         "    if (dc_npcdiag_gate(DC_NPCDIAG_G_MK_GATE, CLIP(npc_clip) != NULL && CLIP(npc_clip)->setupActor_proc != NULL)) {"),

        # 5. The spawn call itself. `slot` counts make entries that held an NPC
        #    name at all, so an empty make list is not read as a failed spawn.
        (1, _lit(
            "                case NAME_TYPE_NPC:\n"
            "                    idx = make_p->idx;\n"
            "                    if (idx != -1) {\n"
            "                        make_ret = CLIP(npc_clip)->setupActor_proc(play, make_p->make_name, idx, idx, -1, make_p->bx, make_p->bz, make_p->ux, make_p->uz);"),
         "                case NAME_TYPE_NPC:\n"
         "                    dc_npcdiag_gate(DC_NPCDIAG_G_MK_SLOT, 1); " + MARK + "\n"
         "                    idx = make_p->idx;\n"
         "                    if (dc_npcdiag_gate(DC_NPCDIAG_G_MK_IDX, idx != -1)) {\n"
         "                        dc_npcdiag_gate(DC_NPCDIAG_G_MK_CALLED, 1); " + MARK + "\n"
         "                        make_ret = dc_npcdiag_gate(DC_NPCDIAG_G_MK_RET, CLIP(npc_clip)->setupActor_proc(play, make_p->make_name, idx, idx, -1, make_p->bx, make_p->bz, make_p->ux, make_p->uz));"),

        # 6. aSNMgr_set_npc_regular's loop and its first two gates. The whole
        #    `for` header is in the anchor because `for (i = 0; i <
        #    ANIMAL_NUM_MAX; i++) {` alone appears eleven times in this file;
        #    the mNpcW_APPEAR_STATUS_REGULAR line is what makes it unique.
        (1, _lit(
            "    for (i = 0; i < ANIMAL_NUM_MAX; i++) {\n"
            "        int set = TRUE;\n"
            "\n"
            "        if (aSNMgr_chk_exist_and_appear(manager, mNpcW_APPEAR_STATUS_REGULAR, i) == TRUE) {\n"
            "            if (aSNMgr_check_in_scope(list_p->position, &manager->scope) == TRUE) {"),
         "    dc_npcdiag_gate(DC_NPCDIAG_G_REG_CALL, 1); " + MARK + "\n"
         "    for (i = 0; i < ANIMAL_NUM_MAX; i++) {\n"
         "        int set = TRUE;\n"
         "\n"
         "        if (dc_npcdiag_gate(DC_NPCDIAG_G_REG_EXIST, aSNMgr_chk_exist_and_appear(manager, mNpcW_APPEAR_STATUS_REGULAR, i) == TRUE)) {\n"
         "            if (dc_npcdiag_gate(DC_NPCDIAG_G_REG_SCOPE, aSNMgr_check_in_scope(list_p->position, &manager->scope) == TRUE)) {"),

        # 7. The REGULAR pass's last three gates.
        (1, _lit(
            "                    aSNMgr_get_block_ut_num_set_npc(&bx, &bz, &ux, &uz, list_p);\n"
            "                    make_flag = aSNMgr_set_appear_info_regular(manager, bx, bz, i);\n"
            "                    if (make_flag == TRUE) {\n"
            "                        if (((joint_event >> i) & 1) == 0) {\n"
            "                            make_flag = aSNMgr_get_safe_utnum(&ux, &uz, bx, bz, *winfo_p);\n"
            "                        }\n"
            "\n"
            "                        if (make_flag == TRUE &&\n"
            "                            aSNMgr_set_make_npc(manager->npc_info.make, animal_p->id.npc_id, bx, bz, ux, uz, info_p, i) != -1) {"),
         "                    aSNMgr_get_block_ut_num_set_npc(&bx, &bz, &ux, &uz, list_p);\n"
         "                    make_flag = dc_npcdiag_gate(DC_NPCDIAG_G_REG_APPEAR, aSNMgr_set_appear_info_regular(manager, bx, bz, i));\n"
         "                    if (make_flag == TRUE) {\n"
         "                        if (((joint_event >> i) & 1) == 0) {\n"
         "                            make_flag = dc_npcdiag_gate(DC_NPCDIAG_G_REG_UTNUM, aSNMgr_get_safe_utnum(&ux, &uz, bx, bz, *winfo_p));\n"
         "                        }\n"
         "\n"
         "                        if (make_flag == TRUE &&\n"
         "                            dc_npcdiag_gate(DC_NPCDIAG_G_REG_MAKE, aSNMgr_set_make_npc(manager->npc_info.make, animal_p->id.npc_id, bx, bz, ux, uz, info_p, i) != -1)) {"),

        # 8. aSNMgr_set_npc_guest — the pass that ACTUALLY RUNS once the player
        #    stops changing acre, because aSNMgr_set_npc_regular's tail switch
        #    installs GUEST on every mFI_WADE_NONE tick. Instrumenting only the
        #    REGULAR pass would have measured the wrong loop. Its two outer
        #    gates count BLOCKED, not passed: they are `!`-tested in the source.
        (1, _lit(
            "    if (!aSNMgr_chk_arbeit_and_demo_and_halloween() &&\n"
            "        !aSNMgr_check_in_block_max(manager->player_pos.now_block[0], manager->player_pos.now_block[1], (u8*)manager->npc_info.in_block_num)) {\n"
            "        for (i = 0; i < ANIMAL_NUM_MAX; i++) {\n"
            "            if (aSNMgr_chk_exist_and_appear_and_event(manager, mNpcW_APPEAR_STATUS_REGULAR, i) == TRUE) {\n"
            "                if (aSNMgr_check_in_scope(list_p->position, scope_p) == TRUE) {\n"
            "                    aSNMgr_get_block_ut_num_set_npc(&bx, &bz, &ux, &uz, list_p);\n"
            "                    if (aSNMgr_set_appear_info_guest(manager, *winfo_p, bx, bz) == TRUE &&\n"
            "                        aSNMgr_get_safe_utnum(&ux, &uz, bx, bz, *winfo_p) == TRUE &&\n"
            "                        aSNMgr_set_make_npc(manager->npc_info.make, animal_p->id.npc_id, bx, bz, ux, uz, info_p, i) != -1) {"),
         "    dc_npcdiag_gate(DC_NPCDIAG_G_GST_CALL, 1); " + MARK + "\n"
         "    if (!dc_npcdiag_gate(DC_NPCDIAG_G_GST_ARBEIT, aSNMgr_chk_arbeit_and_demo_and_halloween()) &&\n"
         "        !dc_npcdiag_gate(DC_NPCDIAG_G_GST_BLKMAX, aSNMgr_check_in_block_max(manager->player_pos.now_block[0], manager->player_pos.now_block[1], (u8*)manager->npc_info.in_block_num))) {\n"
         "        for (i = 0; i < ANIMAL_NUM_MAX; i++) {\n"
         "            if (dc_npcdiag_gate(DC_NPCDIAG_G_GST_EXIST, aSNMgr_chk_exist_and_appear_and_event(manager, mNpcW_APPEAR_STATUS_REGULAR, i) == TRUE)) {\n"
         "                if (dc_npcdiag_gate(DC_NPCDIAG_G_GST_SCOPE, aSNMgr_check_in_scope(list_p->position, scope_p) == TRUE)) {\n"
         "                    aSNMgr_get_block_ut_num_set_npc(&bx, &bz, &ux, &uz, list_p);\n"
         "                    if (dc_npcdiag_gate(DC_NPCDIAG_G_GST_APPEAR, aSNMgr_set_appear_info_guest(manager, *winfo_p, bx, bz) == TRUE) &&\n"
         "                        dc_npcdiag_gate(DC_NPCDIAG_G_GST_UTNUM, aSNMgr_get_safe_utnum(&ux, &uz, bx, bz, *winfo_p) == TRUE) &&\n"
         "                        dc_npcdiag_gate(DC_NPCDIAG_G_GST_MAKE, aSNMgr_set_make_npc(manager->npc_info.make, animal_p->id.npc_id, bx, bz, ux, uz, info_p, i) != -1)) {"),

        # 9. The per-tick sample, at the TOP of aSNMgr_actor_move and BEFORE its
        #    wade switch — so next_block/now_block are still the values the last
        #    set pass actually used, not the ones this tick is about to install.
        #    This is what answers hypothesis 1 (the wade histogram) and
        #    hypothesis 5 (the roster's acres against the player's).
        (1, _lit(
            "static void aSNMgr_actor_move(ACTOR* actorx, GAME* game) {\n"
            "    SET_NPC_MANAGER_ACTOR* manager = (SET_NPC_MANAGER_ACTOR*)actorx;\n"
            "    GAME_PLAY* play = (GAME_PLAY*)game;\n"
            "\n"
            "    aSNMgr_get_player_pos(&manager->player_pos.pos, game);\n"
            "    switch (mFI_GetPlayerWade()) {"),
         "static void aSNMgr_actor_move(ACTOR* actorx, GAME* game) {\n"
         "    SET_NPC_MANAGER_ACTOR* manager = (SET_NPC_MANAGER_ACTOR*)actorx;\n"
         "    GAME_PLAY* play = (GAME_PLAY*)game;\n"
         "\n"
         "    aSNMgr_get_player_pos(&manager->player_pos.pos, game);\n"
         "    dc_npcdiag_tick(mFI_GetPlayerWade(), manager->set_mode,\n"
         "                    manager->player_pos.next_block[0], manager->player_pos.next_block[1],\n"
         "                    manager->player_pos.now_block[0], manager->player_pos.now_block[1],\n"
         "                    manager->npc_info.exist, manager->npc_info.appear,\n"
         "                    manager->npc_info.list_p); " + MARK + "\n"
         "    switch (mFI_GetPlayerWade()) {"),
    ])


def _npcdiag_cloth_rules():
    """src/actor/npc/ac_npc_cloth.c_inc — SHADOW. Only built at --npcdiag=1.

    ⚠️ THIS IS THE ONE THAT MUST NOT BE MISREAD. aNPC_dma_regist_check_cloth_data
    returns FALSE on the FIRST attempt for any cloth BY DESIGN — the DMA
    completes a frame later — so a low `setup: chk` is not evidence of anything
    on its own. What IS decisive is the bank tally: if
    mSc_secure_exchange_keep_bank fails for all ten slots, every `cloth->id`
    stays mSC_BANK_NONE, aNPC_get_new_cloth_data_area can never return a slot,
    and aNPC_setupNpc_check is FALSE forever with no diagnostic at all.
    `cloth=0/10` on the report line is that hypothesis, proved or killed."""
    return ("src/actor/npc/ac_npc_cloth.c_inc", "shadow", [
        # The declarations, before the file's first function. This .c_inc is
        # included BEFORE ac_npc_ctrl.c_inc by both ac_npc.c and ac_npc2.c, so
        # this is the copy of the block that actually survives the
        # DC_NPCDIAG_DECLS_ guard; the one in ac_npc_ctrl.c_inc is the
        # standalone case.
        (1, _lit("static void aNPC_dma_cloth_data(ACTOR* actorx) {"),
         NPCDIAG_HEAD()
         + "static void aNPC_dma_cloth_data(ACTOR* actorx) {"),

        # The whole reservation loop, anchored end to end. Three calls: reset,
        # then one per slot down each arm of the if.
        (1, _lit(
            "    for (i = 0; i < aNPC_CTRL_CLOTH_NUM; i++) {\n"
            "        if (mSc_secure_exchange_keep_bank(exc, 0, aNPC_CLOTH_TEX_SIZE + aNPC_CLOTH_PAL_SIZE) != NULL) {\n"
            "            cloth->texture_bank.size = aNPC_CLOTH_TEX_SIZE;"),
         "    dc_npcdiag_cloth_begin(); " + MARK + "\n"
         "    for (i = 0; i < aNPC_CTRL_CLOTH_NUM; i++) {\n"
         "        if (mSc_secure_exchange_keep_bank(exc, 0, aNPC_CLOTH_TEX_SIZE + aNPC_CLOTH_PAL_SIZE) != NULL) {\n"
         "            dc_npcdiag_cloth_bank(1); " + MARK + "\n"
         "            cloth->texture_bank.size = aNPC_CLOTH_TEX_SIZE;"),
        (1, _lit(
            "        } else {\n"
            "            cloth->texture_bank.ram_start = NULL;\n"
            "            cloth->palette_bank.ram_start = NULL;\n"
            "            cloth->id = mSC_BANK_NONE;"),
         "        } else {\n"
         "            dc_npcdiag_cloth_bank(0); " + MARK + "\n"
         "            cloth->texture_bank.ram_start = NULL;\n"
         "            cloth->palette_bank.ram_start = NULL;\n"
         "            cloth->id = mSC_BANK_NONE;"),
    ])


def _npcdiag_ctrl_rules(diag):
    """The N3 rules that ride S1c's src/actor/npc/ac_npc_ctrl.c_inc entry.

    ⚠️ EMITTED AT --npcdiag=0 TOO, unlike the other two N3 entries. At 0 the
    only thing it contributes is NPCDIAG_GUARD's `#error`, anchored on
    aNPC_setupActor_sub's definition; that is the tripwire for a tree/build
    mismatch, and it has to live in a TU that is in the shrink tree at EVERY
    setting or it could not fire when the tree was generated at 0."""
    sub_body = (
        "static int aNPC_setupActor_sub(GAME_PLAY* play, s8 idx, mActor_name_t name, s16 profile, xyz_t* pos, s16 mvlist_no,\n"
        "                               s16 arg) {\n"
        "    int ret = FALSE;\n"
        "\n"
        "    if (aNPC_setupNpc_check(idx, name) == TRUE) {\n"
        "        if (Actor_info_make_actor(&play->actor_info, (GAME*)play, profile, pos->x, pos->y, pos->z, 0, 0, 0,\n"
        "                                  play->block_table.block_x, play->block_table.block_z, mvlist_no, name, arg, idx,\n"
        "                                  -1) != NULL) {"
    )
    head = NPCDIAG_GUARD(diag) + "\n"
    if not diag:
        # The guard and nothing else: the vendored body goes back verbatim, so
        # --npcdiag=0 leaves this TU byte-identical apart from the #if.
        return [(1, _lit(sub_body), head + sub_body)]

    head += NPCDIAG_HEAD()
    rules = [
        # aNPC_setupActor_sub — the last two gates before an ACTOR exists.
        # `chk` is aNPC_setupNpc_check, i.e. the cloth; `actor` is
        # Actor_info_make_actor, i.e. the actor cap at m_actor.c:760 and the
        # TARGET_PC class_size guard at :765-770, which IS live on this target.
        (1, _lit(sub_body),
         head
         + "static int aNPC_setupActor_sub(GAME_PLAY* play, s8 idx, mActor_name_t name, s16 profile, xyz_t* pos, s16 mvlist_no,\n"
           "                               s16 arg) {\n"
           "    int ret = FALSE;\n"
           "\n"
           "    dc_npcdiag_gate(DC_NPCDIAG_G_SU_ENT, 1); " + MARK + "\n"
           "    if (dc_npcdiag_gate(DC_NPCDIAG_G_SU_CHK, aNPC_setupNpc_check(idx, name) == TRUE)) {\n"
           "        if (dc_npcdiag_gate(DC_NPCDIAG_G_SU_ACTOR, Actor_info_make_actor(&play->actor_info, (GAME*)play, profile, pos->x, pos->y, pos->z, 0, 0, 0,\n"
           "                                  play->block_table.block_x, play->block_table.block_z, mvlist_no, name, arg, idx,\n"
           "                                  -1) != NULL)) {"),

        # aNPC_actor_ct_c — the ONLY place CLIP(npc_clip) is ever assigned, and
        # the frame in which aNPC_keep_cloth_data_area has just finished, so the
        # cloth tally is final one statement later. Two one-shots: the
        # controller ran, and the clip was installed.
        (1, _lit(
            "    aNPC_ctrlActor = actorx;\n"
            "    aNPC_keep_cloth_data_area(obj_ex);\n"
            "\n"
            "    if (CLIP(npc_clip) == NULL) {\n"
            "        CLIP(npc_clip) = &aNPC_clip;\n"
            "        bzero(&aNPC_clip, sizeof(aNPC_clip));"),
         "    aNPC_ctrlActor = actorx;\n"
         "    aNPC_keep_cloth_data_area(obj_ex);\n"
         "    dc_npcdiag_ctrl_ct(DC_NPCDIAG_IS_NPC2, CLIP(npc_clip) != NULL); " + MARK + "\n"
         "\n"
         "    if (CLIP(npc_clip) == NULL) {\n"
         "        CLIP(npc_clip) = &aNPC_clip;\n"
         "        bzero(&aNPC_clip, sizeof(aNPC_clip));\n"
         "        dc_npcdiag_clip_set(DC_NPCDIAG_IS_NPC2); " + MARK),
    ]
    return rules


def NPCSEED_GUARD(seed):
    """N2c's stale-tree guard. Weaker than S11/S12's on purpose: dc_npc_seed()
    is defined unconditionally in dc/src/dc_npcseed.c (empty at
    DC_NPC_SEED=0), so a mismatched tree cannot fail to LINK — it fails
    SILENTLY, by injecting a call to a function that does nothing, or by
    defining a working seeder that nothing calls. Both directions are worth an
    #error for exactly that reason: the failure has no symptom of its own, it
    just looks like "the fix did not work"."""
    if seed:
        return (
            "#if defined(DC_NPC_SEED) && !DC_NPC_SEED\n"
            "#error \"DC_SRC_SHRINK N2c: dc/build/shrinksrc was generated with "
            "--npc-seed=1, so mSDI_StartInitAfter calls dc_npc_seed() -- but "
            "this build defines DC_NPC_SEED=0, where that function is empty. "
            "Re-run tools/dcstub/make_src_shrink.py with --npc-seed=0 (or "
            "rm -rf dc/build/shrinksrc) before building.\"\n"
            "#endif")
    return (
        "#if defined(DC_NPC_SEED) && DC_NPC_SEED\n"
        "#error \"DC_SRC_SHRINK N2c: dc/build/shrinksrc was generated with "
        "--npc-seed=0, so nothing calls dc_npc_seed() -- but this build "
        "defines DC_NPC_SEED nonzero, compiling a seeder nothing would call. "
        "Re-run tools/dcstub/make_src_shrink.py with --npc-seed=1 (or "
        "rm -rf dc/build/shrinksrc) before building.\"\n"
        "#endif")


def _s11_reset_rules(npctex_pool, npcmdl_pool, npc_seed=0):
    """The two RESET sites, shared by S11 (R2) and S12 (R3). SWAPS: both are .c
    files compiled directly, so -I cannot reach them, and neither is in
    stub.list (main() hard-errors if that ever changes).

    Each entry is two rules — an anchor at the enclosing function's opening
    brace that introduces the guards and the externs, and the calls themselves
    right after mNpc_SetNpcList(). Anchoring the call on its full argument list
    is what keeps this from silently landing on the ISLAND callers, which take a
    different list and must NOT reset.

    ⚠️ N2c (dc_npc_seed) RIDES THIS ENTRY RATHER THAN ADDING ITS OWN, and that
    is forced, not stylistic: main() raises SystemExit if one path appears in
    two rule entries, because the second write would overwrite the first. So
    the seeder's prototype goes in head() and its call goes in the SAME
    replacement as the pool resets — before mNpc_SetNpcList, where the resets
    are after it. The order matters in both directions: the pools must be reset
    only once no NPC actor can exist (after the list is rebuilt), and the seed
    must land before the list is built from it.

    ⚠️ THE TRADEMARK ENTRY DOES NOT GET THE SEEDER. set_npc_4_title_demo
    already fills animals[] from demo_npc_list via mNpc_SetAnimalTitleDemo, so
    seeding there would be a no-op at best; and its mNpc_SetNpcList call passes
    demo_npc_num, not ANIMAL_NUM_MAX, i.e. it is a different roster with a
    different length. Only the game-start site is seeded.
    """
    def head(what):
        h = NPCTEX_GUARD(npctex_pool) + "\n"
        if npctex_pool:
            h += (
                "/* DC_SRC_SHRINK S11: R2's RESET seam (%s). The villager\n"
                " * texture pool never evicts — an actor keeps a private copy of\n"
                " * the 16 texture pointers (ac_npc_ct.c_inc:256), so reusing a\n"
                " * slot under a live actor swaps one animal's skin onto\n"
                " * another. This is one of exactly TWO points that rebuild the\n"
                " * whole npclist AND are between scene teardowns, i.e. where no\n"
                " * NPC actor can exist. Do not move it and do not add a third.\n"
                " * dc/src/dc_npctex.c carries the whole argument. */\n"
                "extern void dc_npctex_pool_reset(void);\n" % what)
        h += NPCMDL_GUARD(npcmdl_pool) + "\n"
        if npcmdl_pool:
            h += (
                "/* DC_SRC_SHRINK S12: R3's RESET seam, the same point and the\n"
                " * same rule. The MODEL pool never evicts either, and for a\n"
                " * harder reason: a species' display lists are GLOBAL, so\n"
                " * reusing a slot would put one animal's vertices into every\n"
                " * live actor of another, mid-frame. The reset also puts the\n"
                " * 933 patched gsSPVertex words back to the stub arrays they\n"
                " * pointed at when linked. dc/src/dc_npcmdl.c carries the\n"
                " * argument. */\n"
                "extern void dc_npcmdl_pool_reset(void);\n")
        return h

    def calls():
        out = []
        if npctex_pool:
            out.append("    dc_npctex_pool_reset(); " + MARK)
        if npcmdl_pool:
            out.append("    dc_npcmdl_pool_reset(); " + MARK)
        if not out:
            out.append("    /* DC_NPCTEX_POOL=0, DC_NPCMDL_POOL=0: no pool to"
                       " reset. */")
        return "\n".join(out)

    def seed_head():
        h = NPCSEED_GUARD(npc_seed) + "\n"
        if npc_seed:
            h += (
                "/* DC_SRC_SHRINK N2c: the villager SEED seam. animals[] is not\n"
                " * loaded from the save on a new game -- it is generated by\n"
                " * mNpc_DecideLivingNpcMax -- but home_info stays 0xFF unless\n"
                " * mNpc_SetNpcHome finds SIGN00..SIGN20 reserve markers in\n"
                " * Save_Get(fg[][]), and mNpc_SetNpcList silently skips every\n"
                " * slot whose block_x is 0xFF. This runs immediately BEFORE\n"
                " * that call and repairs only what is actually missing.\n"
                " * dc/src/dc_npcseed.c carries the whole argument. */\n"
                "extern void dc_npc_seed(void);\n")
        return h

    def seed_call():
        if npc_seed:
            return "    dc_npc_seed(); " + MARK + "\n"
        return ""

    out = []
    out.append(("src/game/m_start_data_init.c", "swap", [
        (1,
         r"^extern void mSDI_StartInitAfter\(GAME\* game, int renew_mode,"
         r" int malloc_flag\) \{$",
         head("mSDI_StartInitAfter — player-select / game start")
         + seed_head()
         + "extern void mSDI_StartInitAfter(GAME* game, int renew_mode,"
           " int malloc_flag) {"),
        (1,
         r"^    mNpc_SetNpcList\(Common_Get\(npclist\), animals, ANIMAL_NUM_MAX,"
         r" malloc_flag\);$",
         seed_call()
         + "    mNpc_SetNpcList(Common_Get(npclist), animals, ANIMAL_NUM_MAX,"
         " malloc_flag);\n" + calls()),
    ]))
    out.append(("src/game/m_trademark.c", "swap", [
        (1,
         r"^static int set_npc_4_title_demo\(GAME_TRADEMARK\* trademark\) \{$",
         head("set_npc_4_title_demo — the 14 title-demo villagers")
         + "static int set_npc_4_title_demo(GAME_TRADEMARK* trademark) {"),
        (1,
         r"^    mNpc_SetNpcList\(Common_Get\(npclist\), animals, demo_npc_num,"
         r" 0\);$",
         "    mNpc_SetNpcList(Common_Get(npclist), animals, demo_npc_num, 0);\n"
         + calls()),
    ]))
    return out


def _s7_rules(bgtex_demand):
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

    # S10 rides here, not in an entry of its own: one output file per TU, so a
    # second entry for this path would overwrite the first (the `seen` check in
    # main() is what keeps that honest). Appended last — none of its anchors is
    # touched by the three rules above.
    fm_rules += _s10_rules(bgtex_demand)

    return [(S7_BGD, "swap", bgd_rules), (S7_FM, "swap", fm_rules)]


# ---------------------------------------------------------------------------
# S9 / S8 — the jaudio_NES staging buffers and pools.
# ---------------------------------------------------------------------------
# These are built by a FUNCTION rather than spelled into RULES, because one of
# them (dvdthread.c) carries an unconditional half and a conditional half and
# `RULES` is keyed by path with ONE output file per entry. Two entries for the
# same path would silently write the file twice and keep whichever ran last.

# The stale-tree guard. dc/build/shrinksrc survives `make` and survives a flag
# flip; `dc/build/flags.stamp` forces a REBUILD when DC_AUDIO changes but it
# cannot regenerate the scratch tree, and the Makefile compiles whatever tree is
# on disk. So the tree has to say what it was generated for, in a form the
# compiler refuses to ignore.
#
# `defined(DC_AUDIO) && DC_AUDIO` and not plain `DC_AUDIO`: dc/Makefile only
# passes -DDC_AUDIO=1 when sound is on and passes nothing at all when it is off,
# so the undefined case must be the quiet one.
def AUDIO_OFF_GUARD(what):
    return (
        "/* " + "-" * 68 + "\n"
        " * DC_SRC_SHRINK S8 — THIS COPY WAS GENERATED FOR DC_AUDIO=0.\n"
        " * " + what + "\n"
        " * The pool is only safe at this size because the per-frame chain that\n"
        " * fills it is compiled out with the pump (dc/src/dc_audio.c:497). Turn\n"
        " * sound back on and it is a live buffer again, so refuse to build.\n"
        " * " + "-" * 68 + " */\n"
        "#if defined(DC_AUDIO) && DC_AUDIO\n"
        "#error \"DC_SRC_SHRINK S8: dc/build/shrinksrc was generated with "
        "--audio=0, but this build defines DC_AUDIO=1. Re-run "
        "tools/dcstub/make_src_shrink.py --audio=1 (or rm -rf dc/build/shrinksrc) "
        "before building with sound.\"\n"
        "#endif")


def _audio_rules(audio):
    """S9 (always) + S8 (only when audio is off). Returns RULES-shaped entries."""

    # -- S9a: heapctrl.c DMABUFFER_SIZE ------------------------------------
    # SWAP; heapctrl.c is compiled directly so -I cannot reach it, and the macro
    # is file-local (grep: three uses, all in this file — :10 the array, :43 and
    # :74 the two burst clamps) with no #undef anywhere in the tree.
    #
    # Both users have the shape `burst = total >= DMABUFFER_SIZE ?
    # DMABUFFER_SIZE : total`, so the buffer and the clamp move together and the
    # loop simply runs 16x more often. :74's staging area is JAC_ARAM_DMA_BUFFER_TOP
    # in ARAM, not `dmabuffer` at all — it only borrows the constant — so a
    # smaller burst there is strictly less ARAM touched.
    #
    # ⚠️ MEASURED SAVING: ZERO, and that is the honest number. dc/build/gcdrop.txt
    # carries `removing unused section '.bss.dmabuffer'` and
    # `removing unused section '.text.Jac_GarbageCollection_St'` — the array's
    # only reachable user is that collector, whose own single caller
    # (aramcall.c:87) is unreferenced, so --gc-sections already deletes all
    # 65,536 B. `sh-elf-nm` on the linked ELF has no `_dmabuffer`. The rule stays
    # for one reason: a --no-gc-sections debug link (or any future reference to
    # the collector) puts the buffer straight back, and 4 KB is the right size
    # for it on a 16 MB machine either way. It is NOT counted in EXPECTED.
    out = [
        ("src/static/jaudio_NES/internal/heapctrl.c", "swap", [
            (1, r"^#define DMABUFFER_SIZE \(0x10000\)$",
             "/* DC_SRC_SHRINK S9a: 65,536 -> 4,096 B. Both consumers clamp their\n"
             " * burst to this same macro (:43, :74), so the only effect is 16x more\n"
             " * iterations of two loops that, on Dreamcast, run zero times: the ARQ\n"
             " * garbage collector this buffer serves is already dropped by\n"
             " * --gc-sections. Worth 0 B today; worth 61,440 B the moment anything\n"
             " * references Jac_GarbageCollection_St again. */\n"
             "#define DMABUFFER_SIZE (0x1000)"),
        ]),
    ]

    # -- S9b + S8c: dvdthread.c ---------------------------------------------
    # ONE entry, two rewrites of different lifetimes. SWAP: dvdthread.c is
    # compiled directly.
    #
    # S9b (unconditional) — dvd_buf is the ADVD_BUFFER backing store, and the
    # ONLY thing that ever sizes it is the __WriteBufferSize call on the very
    # next line: 2 buffers x 0x8000 = exactly 0x10000. Its one reader,
    # DVDT_LoadtoARAM_Main, reads `min(remaining, buffersize)` per pass
    # (:176-185), so halving the pair twice just quadruples the pass count.
    # The align-up branch at :177 can round a short tail up to at most
    # `buffersize` itself (buffersize is 32-aligned), so it still cannot leave
    # the buffer.
    #
    # THE PAIR IS ATOMIC. Shrinking the array alone is the exact kb/traps.md
    # "full-size pass over a shrunken destination" shape that cost a debug cycle
    # under DC_ASSET_STUB: buffersize would stay 0x8000, ADVD_BUFFER[1] would
    # point 0x8000 into a 0x4000 array, and every read would land in whatever
    # .bss follows.
    #
    # Note the GC `#else` block has its own `__WriteBufferSize(buf, 2, 0x8000);`
    # at :372 — different first argument, so the anchor below cannot reach it,
    # and it is not compiled anyway.
    #
    # FOOTPRINT NOTE, not a correctness one: today this costs no extra disc I/O
    # at all. `pc_dvd_process_all_tasks()` is called exactly once in the whole
    # program (neosthread.c:86) and the one task queued by then is a
    # DVDT_LoadtoARAM of "/audiorom.img", which nothing stages onto the disc —
    # Jac_DVDOpen fails and __DoError returns before a single read. If audiorom
    # is ever staged, this quadruples the number of GD-ROM reads for it; both
    # sizes stay 2048-aligned, so the kb/traps.md two-command penalty for an
    # unaligned read does not apply.
    dvd = [
        (1, r"^    static u8 dvd_buf\[0x10000\];$",
         "    /* DC_SRC_SHRINK S9b: 65,536 -> 16,384 B. This pair is ATOMIC — the\n"
         "     * next line is the ONLY thing that tells the reader how big each of\n"
         "     * the two buffers is, and DVDT_LoadtoARAM_Main reads exactly\n"
         "     * `buffersize` bytes per pass. Shrink one without the other and\n"
         "     * ADVD_BUFFER[1] points past the end of the array. */\n"
         "    static u8 dvd_buf[0x4000];"),
        (1, r"^    __WriteBufferSize\(dvd_buf, 2, 0x8000\);$",
         "    __WriteBufferSize(dvd_buf, 2, 0x2000); " + MARK),
    ]

    if not audio:
        # -- S8c: CALLSTACK + its modulus ----------------------------------
        # 128 slots x 256 B of DVD task frames, of which the program uses ONE:
        # `mq_init` is still 0 when Jac_InitARAM queues its transfers, so
        # DVDT_AddTaskHigh returns 0 without touching CALLSTACK; jac_dvdproc_init
        # then sets mq_init, pc_neos_init_sync queues one task, and
        # pc_dvd_process_all_tasks drains it. That is the only drain in the
        # program, so everything queued afterwards is never executed and its
        # frame contents never read. 16 slots is 16x the observed peak.
        #
        # THE PAIR IS ATOMIC for the same reason as S9b: GetCallStack indexes
        # `CALLSTACK[cur_q * 0x100]` and the modulus is what keeps cur_q inside
        # the array. 0x1000 / 0x100 = 0x10. A frame is 4 + 0x58 = 92 B, well
        # inside the 256 B slot, so the slot SIZE is untouched.
        #
        # ⚠️ THE DECLARATION IS ANCHORED TOGETHER WITH `static u32 cur_q = 0;`
        # ON PURPOSE. `static u8 CALLSTACK[0x8000];` appears TWICE in this file,
        # byte-identically — :29 in the TARGET_PC block and :259 in the
        # GameCube `#else`. The GC copy is followed by a blank line and
        # `static u32 mq_init;`, and its own `cur_q` is uninitialised
        # (`static u32 cur_q;` at :267), so the two-line anchor matches exactly
        # one of them. A bare one-line pattern would match both and the
        # must_match count would not notice, because it would be 2 either way.
        dvd += [
            (1, r'^#include "jaudio_NES/dvdthread.h"$',
             '#include "jaudio_NES/dvdthread.h"\n\n'
             + AUDIO_OFF_GUARD("CALLSTACK is 16 task frames here, not 128.")),
            (1,
             r"^static u8 CALLSTACK\[0x8000\];\n"
             r"static u32 cur_q = 0;$",
             "/* DC_SRC_SHRINK S8c: 32,768 -> 4,096 B = 16 frames of 256 B. The\n"
             " * modulus in GetCallStack() below moves with it (0x1000/0x100 =\n"
             " * 0x10); they are one change, not two. Only ONE frame is ever live:\n"
             " * pc_dvd_process_all_tasks() runs exactly once (neosthread.c:86) and\n"
             " * drains the single task queued before it. */\n"
             "static u8 CALLSTACK[0x1000];\n"
             "static u32 cur_q = 0;"),
            (1, r"^    cur_q = \(cur_q \+ 1\) % 0x80;$",
             "    cur_q = (cur_q + 1) % 0x10; " + MARK),
        ]

    out.append(("src/static/jaudio_NES/internal/dvdthread.c", "swap", dvd))

    if audio:
        # -- S8e: neosthread.c, the RSP command buffer is TOO SMALL with sound
        #    ON, and nothing in the program checks. This is the only rule in
        #    this file that GROWS a buffer, and it is a correctness fix, not a
        #    saving.
        #
        # jaudio sizes its own Acmd buffer from a bound it computes at spec
        # time (memory.c:1029):
        #
        #     max_audio_cmds = num_channels * 20 * updates_per_frame
        #                      + spec->_09 * 30 + 400
        #                    = 24 * 20 * 4 + 1 * 30 + 400
        #                    = 2,350
        #
        # (num_channels = NA_SPEC_CONFIG[0]._05 = 24, audioconst.c:9;
        #  updates_per_frame = 4, memory.c:971, x spec = _04 = 1 at :1021;
        #  _09 = 1, audioconst.c:13.)
        #
        # On N64 that bound sizes AG.abi_cmd_bufs[] (memory.c:1061). THE PC /
        # DC PATH DOES NOT USE THOSE BUFFERS — Neos_Update calls
        # CreateAudioTask(pc_task_buf[cur], …) (neosthread.c:35) into a FIXED
        # `Acmd pc_task_buf[2][1600]` (:26) — and `max_audio_cmds` appears
        # nowhere else in the tree, so Nas_smzAudioFrame writes commands with
        # no bound of any kind. At the engine's own worst case that is 750
        # Acmds = 6,000 B past the end: from pc_task_buf[0] it corrupts
        # pc_task_buf[1], and from pc_task_buf[1] it corrupts whatever .bss
        # follows.
        #
        # ⚠️ WHY IT HAS NEVER BEEN SEEN: the command count scales with ACTIVE
        # VOICES, and until the port-queue drain landed (dc/src/dc_audio.c) the
        # music never started, so the port had only ever run 0-4 concurrent
        # voices. Turning the music on is exactly the change that walks this
        # buffer into its bound. Sizing it correctly BEFORE the first
        # music-playing run is the cheap half of that bet.
        #
        # 2,432 = 2,350 rounded up to a multiple of 64, so the pair costs
        # 2 x (2432 - 1600) x 8 = 13,312 B of .bss against ~2.05 MB of
        # headroom. Lowering DC_AUDIO_VOICES (L1) lowers the bound and leaves
        # this merely generous, never short.
        #
        # The macro also sizes the GameCube `task_buf[2][1600]` at :163 inside
        # the `#else`, which is not compiled here.
        out.append(("src/static/jaudio_NES/internal/neosthread.c", "swap", [
            (1, r"^#define NEOSTHREAD_ACMD_BUF_NUM 1600$",
             "/* DC_SRC_SHRINK S8e: 1,600 -> 2,432 Acmds (+13,312 B of .bss).\n"
             " * jaudio's own bound is max_audio_cmds = num_channels*20*\n"
             " * updates_per_frame + _09*30 + 400 = 24*20*4 + 30 + 400 = 2,350\n"
             " * (memory.c:1029), and the DC path writes into this fixed array\n"
             " * rather than into the AG.abi_cmd_bufs[] that bound sizes\n"
             " * (neosthread.c:35 vs memory.c:1061). Nas_smzAudioFrame checks\n"
             " * nothing, so at 1,600 a full voice load runs 6,000 B off the end.\n"
             " * Only reachable with sound ON, which is why the --audio=0 tree\n"
             " * shrinks this same macro to 16 instead (S8d). */\n"
             "#define NEOSTHREAD_ACMD_BUF_NUM 2432"),
        ]))
        return out

    # -- S8a: seqsetup.c, the sequencer track pool --------------------------
    # 256 x 1,076 B of seqp_ plus a 256-entry free queue. SWAP; both macros and
    # both arrays are file-local statics (grep: SEQ_SIZE and FREE_SEQP_QUEUE_SIZE
    # appear nowhere else in src/ or include/).
    #
    # THEY MUST STAY EQUAL. Jaq_Reset fills `FREE_SEQP_QUEUE[i] = &seq[i]` for
    # i < SEQ_SIZE and then sets `SEQ_REMAIN = FREE_SEQP_QUEUE_SIZE`
    # (seqsetup.c:37-46). Make the queue longer than the pool and SEQ_REMAIN
    # claims free tracks that were never written, so GetNewTrack hands out
    # uninitialised pointers. Make it shorter and Jaq_Reset writes past its end.
    # One number, spelled twice, in the vendored source.
    #
    # ROOTSEQ_SIZE and ROOT_OUTER_SIZE are DELIBERATELY LEFT AT 16. rootseq is
    # 64 B and ROOT_OUTER is 1,024 B — nothing worth having — and `handle` is an
    # index into rootseq that Jaq_HandleToSeq (:162) dereferences unchecked.
    out.append(("src/static/jaudio_NES/internal/seqsetup.c", "swap", [
        (1, r'^#include "jaudio_NES/seqsetup.h"$',
         '#include "jaudio_NES/seqsetup.h"\n\n'
         + AUDIO_OFF_GUARD("seq[] is 8 tracks here, not 256.")),
        (1, r"^#define SEQ_SIZE             \(256\)$",
         "/* DC_SRC_SHRINK S8a: 256 -> 8 tracks (275,456 -> 8,608 B of seqp_).\n"
         " * SEQ_SIZE and FREE_SEQP_QUEUE_SIZE ARE THE SAME NUMBER and Jaq_Reset\n"
         " * below depends on it: it fills the queue with SEQ_SIZE entries and then\n"
         " * declares FREE_SEQP_QUEUE_SIZE of them free. Change one alone and\n"
         " * GetNewTrack() starts returning pointers that were never written.\n"
         " * ROOTSEQ_SIZE / ROOT_OUTER_SIZE stay at 16: they are 64 B and 1,024 B,\n"
         " * and a root handle indexes rootseq[] without a bounds check (:162). */\n"
         "#define SEQ_SIZE             (8)"),
        # NB: no `+ MARK` here — MARK is itself a /* … */ and nesting it inside
        # a trailing comment produces `/* … /* DC_SRC_SHRINK */ */`, whose stray
        # closing token is a syntax error, not a warning.
        (1, r"^#define FREE_SEQP_QUEUE_SIZE \(256\)$",
         "#define FREE_SEQP_QUEUE_SIZE (8)   "
         "/* == SEQ_SIZE, always. DC_SRC_SHRINK S8a */"),
    ]))

    # -- S8b: driverinterface.c, the logical-channel pool --------------------
    # 256 x 320 B of jc_. SWAP; CHANNEL_SIZE is file-local and every use scales
    # with it — :13 the array, :497 the init loop that seeds the global free
    # list, :503 `global_channel->chanCount = CHANNEL_SIZE`, and :743
    # KillBrokenLogicalChannels' sweep. There is no separate hard-coded 256
    # anywhere: the channel COUNT and the array LENGTH are the one macro.
    #
    # Nothing under-runs when the pool is small. FixAllocChannel (:203) walks
    # `while (num < size)` but breaks the moment List_GetChannel returns NULL and
    # only accounts for the `num` it actually got, so chanCount cannot go
    # negative; its two callers are Init_1shot and a note-on path, both of which
    # tolerate getting fewer channels than they asked for.
    out.append(("src/static/jaudio_NES/internal/driverinterface.c", "swap", [
        (1, r'^#include "jaudio_NES/driverinterface.h"$',
         '#include "jaudio_NES/driverinterface.h"\n\n'
         + AUDIO_OFF_GUARD("CHANNEL[] is 8 logical channels here, not 256.")),
        (1, r"^#define CHANNEL_SIZE \(0x100\)$",
         "/* DC_SRC_SHRINK S8b: 256 -> 8 logical channels (81,920 -> 2,560 B).\n"
         " * Every use scales with the macro — the array (:13), InitGlobalChannel's\n"
         " * seeding loop and its chanCount (:497, :503), and\n"
         " * KillBrokenLogicalChannels' sweep (:743) — so there is no second,\n"
         " * hard-coded 256 left behind. FixAllocChannel breaks on an empty free\n"
         " * list and counts only what it got, so a short pool degrades to fewer\n"
         " * voices rather than to a negative chanCount. */\n"
         "#define CHANNEL_SIZE (0x8)"),
    ]))

    # -- S8d: neosthread.c, the RSP command buffer ---------------------------
    # pc_task_buf[2][1600] of Acmd (8 B) = 25,600 B. SWAP.
    #
    # Its ONLY writer is CreateAudioTask(pc_task_buf[cur], …) at :35, inside
    # Neos_Update, whose only call site in the tree is cpubuf.c:49 — reached only
    # from CpubufProcess(FRAME_END), i.e. only from the Jac_VframeWork tick. With
    # the pump compiled out that call never happens, so the buffer is written
    # exactly never and 16 entries is 16 more than are needed.
    #
    # The macro also sizes the GameCube `task_buf[2][1600]` at :163, inside the
    # `#else`. It shrinks too and is not compiled; harmless either way.
    out.append(("src/static/jaudio_NES/internal/neosthread.c", "swap", [
        (1, r'^#include "jaudio_NES/neosthread.h"$',
         '#include "jaudio_NES/neosthread.h"\n\n'
         + AUDIO_OFF_GUARD("pc_task_buf holds 16 Acmds here, not 1,600.")),
        (1, r"^#define NEOSTHREAD_ACMD_BUF_NUM 1600$",
         "/* DC_SRC_SHRINK S8d: 1,600 -> 16 Acmds (2 x 1600 x 8 = 25,600 -> 256 B).\n"
         " * The buffer's only writer is CreateAudioTask() at :35, in Neos_Update,\n"
         " * whose only caller is cpubuf.c:49 on the CpubufProcess(FRAME_END) path —\n"
         " * i.e. only from the Jac_VframeWork tick, which DC_AUDIO=0 removes. */\n"
         "#define NEOSTHREAD_ACMD_BUF_NUM 16"),
    ]))

    return out


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
    # S9b. Unconditional — correct with sound on too.
    ("dvd_buf",              65536, 16384),
    # S9a (dmabuffer, 65536 -> 4096) is deliberately ABSENT: --gc-sections
    # already deletes the whole section, so the honest linked-image delta is 0.
    # See the rule.
]

# S8 — only present when the tree is generated with --audio=0. Same source as
# EXPECTED above (`sh-elf-nm -S` on the clean non-stub ELF); sizes divide
# exactly, which is how sizeof(seqp_) == 1076 and sizeof(jc_) == 320 were
# confirmed rather than assumed.
EXPECTED_AUDIO_OFF = [
    ("seq",                 275456,  8608),   # 256 -> 8 x 1076
    ("FREE_SEQP_QUEUE",       1024,    32),   # 256 -> 8 x 4
    ("CHANNEL",              81920,  2560),   # 256 -> 8 x 320
    ("CALLSTACK",            32768,  4096),   # 128 -> 16 x 256
    ("pc_task_buf",          25600,   256),   # 2 x 1600 -> 2 x 16 x 8
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


# ---------------------------------------------------------------------------
# P3 — rspsim.c: take the floating point out of A_CMD_ENVMIXER.
# ---------------------------------------------------------------------------
# ⭐ MEASURED ON REAL HARDWARE, 2026-08-12. The first gprof profile off a retail
# Dreamcast put `RspStart` at 15.7 % of non-idle / ~21 % of real work — the
# second-largest symbol on the machine and 2.18x its Flycast share, i.e. the one
# subsystem that genuinely gets WORSE on silicon. Localising samples to source
# lines through the histogram's 8-byte bins put 11.4 % of RspStart on these four
# declarations and the four adds that consume them.
#
# THE CHANGE IS BIT-EXACT, AND THAT IS PROVABLE RATHER THAN HOPED:
#   var_r4_7 and friends are s16-derived, so |x| <= 32768.
#   envParam1_0 is a u16, so |x * envParam1_0| < 2^31.
#   The three terms are >>16, >>18, >>19, so their sum is < 46,000.
#   var_r20[] is s16, so the final sum is < 78,000 < 2^24.
# Every value is therefore exactly representable in an f32, and truncating an
# exactly-represented integer back to s32 is the identity. No rounding mode, no
# denormal, no edge case — this is a decomp artifact, not a design decision.
# kb/audio-cpu-cost.md §8 suspected it; the hardware profile priced it.
#
# WHY A SHRINK RULE AND NOT AN EDIT: CLAUDE.md §1 — `src/` is never edited to
# make it compile or to make it fast. The whole decomp tree carries five
# TARGET_DC branches and this does not deserve a sixth.
#
# KILL SWITCH: --rspsim-nofp=0 emits no rule at all, so rspsim.c is not swapped,
# nothing lands in shrink.list, and the build is byte-identical to before.
def _rspsim_rules(nofp):
    if not nofp:
        return []
    return [("src/static/jaudio_NES/internal/rspsim.c", "swap", [
        (4,
         r"^(\s*)f32 (var_f[2456]) = ",
         lambda m: "%ss32 %s = " % (m.group(1), m.group(2))),
    ])]


# ---------------------------------------------------------------------------
# W1 — audioheaders.c: point the wave-bank tables at an 8-bit PCM audiorom.
# ---------------------------------------------------------------------------
# `A_CMD_ADPCM` (rspsim.c:107) costs order 10 s16 multiply-accumulates per
# sample; `A_CMD_PCM8DEC` (rspsim.c:591) is `*dst++ = (*src++) << 8`. The kb
# puts ~29 % of `RspStart` on ADPCM decode and `RspStart` is 18.9 % of busy CPU
# on real silicon -- roughly 4.2 ms of a 76.4 ms frame.
#
# `CODEC_S8` is a complete first-class path in the shipped engine (driver.c:815
# skips the codebook load for non-ADPCM, :907 sets the frame geometry, :1043
# emits Nas_PCM8dec) and the codec is a BITFIELD IN THE ROM DATA. So the whole
# change is data -- `tools/dcaudio/s8bank.py` rewrites audiorom.img -- except
# for the index tables, which are compiled into the game. This rule moves them.
#
# 🔴 THE IMAGE AND THESE TABLES ARE TWO ARTEFACTS THAT MUST AGREE. If they
# drift, the game reads sample data at the wrong offsets and plays NOISE rather
# than failing, which is indistinguishable from a codec bug. Two defences:
#   - the numbers are READ FROM s8bank.py's JSON, never retyped here;
#   - dc/src/dc_audio.c checks audiorom.img's real size against the compiled
#     total at boot and refuses loudly on a mismatch (DC_AUDIO_S8_BYTES).
#
# KILL SWITCH: --audio-s8=0 (the default) emits no rule at all, audioheaders.c
# is not swapped, and the build is byte-identical to before.
def _audio_s8_rules(tables):
    if not tables:
        return []

    wave = [tuple(w) for w in tables["wave"]]
    wave_old = [tuple(w) for w in tables["wave_old"]]
    tbl_size = int(tables["tbl_size"])

    # Both tables are machine-generated and rigidly regular: every entry lists
    # `0xXXXXXXXX,  /* rom addr */` then `0xXXXXXXXX,  /* size */`. Rewriting
    # those two fields in document order preserves medium/cache/param exactly,
    # which regenerating the block would not.
    FIELD = r"(0x[0-9A-Fa-f]{8},)(\s*/\* (?:rom addr|size) \*/)"

    def _retable(name, new_pairs, old_pairs):
        def repl(m):
            body = m.group(0)
            want = []
            for (a, s) in new_pairs:
                want.append(a)
                want.append(s)
            got_old = []

            def one(mm):
                i = len(got_old)
                got_old.append(int(mm.group(1)[:-1], 16))
                if i >= len(want):
                    raise RuleError(
                        "%s: more addr/size fields than the JSON describes "
                        "(%d entries)" % (name, len(new_pairs)))
                return "0x%08X,%s" % (want[i], mm.group(2))

            out = re.sub(FIELD, one, body)
            if len(got_old) != len(want):
                raise RuleError(
                    "%s: found %d addr/size fields, expected %d"
                    % (name, len(got_old), len(want)))
            # The vendored table must still be the one these numbers were
            # derived from. Anything else means audiorom.img and src/ have
            # drifted apart, which is the one failure this rule must not pass.
            flat_old = []
            for (a, s) in old_pairs:
                flat_old.append(a)
                flat_old.append(s)
            if got_old != flat_old:
                raise RuleError(
                    "%s: the vendored table does not match `wave_old` in the\n"
                    "  JSON. audiorom.img and src/ have drifted -- regenerate\n"
                    "  with tools/dcaudio/s8bank.py.\n"
                    "    in src/: %s\n"
                    "    in JSON: %s" % (name, got_old, flat_old))
            return out
        return repl

    # AudiodataHeaderStart's three extents: seq and ctl keep BOTH their offset
    # and their size (s8bank.py patches the ctl in place), so only the tbl
    # extent's size moves.
    data_old = [tuple(tables["seq_extent"]), tuple(tables["ctl_extent"]),
                (int(tables["tbl_base"]), 7025632)]
    data_new = [tuple(tables["seq_extent"]), tuple(tables["ctl_extent"]),
                (int(tables["tbl_base"]), tbl_size)]

    return [("src/static/jaudio_NES/game/audioheaders.c", "swap", [
        (1,
         r"(?s)ArcHeader AudiowaveHeaderStart\b.*?\n\};",
         _retable("AudiowaveHeaderStart", wave, wave_old)),
        (1,
         r"(?s)ArcHeader AudiodataHeaderStart\b.*?\n\};",
         _retable("AudiodataHeaderStart", data_new, data_old)),
    ])]


def _load_s8_tables(arg):
    """Read s8bank.py's JSON, or None when W1 is off."""
    if not arg:
        return None
    p = Path(arg)
    if not p.is_absolute():
        p = REPO / p
    if not p.exists():
        raise SystemExit(
            "make_src_shrink: DC_AUDIO_S8=1 but %s is missing.\n"
            "  Generate it with:\n"
            "    python3 tools/dcaudio/s8bank.py --out <discroot>/audiorom.img "
            "--tables %s" % (p, arg))
    with open(p, "r", encoding="utf-8") as fh:
        return json.load(fh)


def main():
    ap = argparse.ArgumentParser(description="Build the DC_SRC_SHRINK tree.")
    ap.add_argument("--rspsim-nofp", type=int,
                    default=int(os.environ.get("DC_RSPSIM_NOFP", "1") or "1"),
                    choices=(0, 1),
                    help="P3. 1 (the default) retypes A_CMD_ENVMIXER's four "
                         "f32 accumulators to s32 in rspsim.c. The values are "
                         "provably < 2^24 so the rewrite is BIT-EXACT, not an "
                         "approximation. 0 emits no rule and rspsim.c is not "
                         "swapped at all -- a byte-identical revert. Measured "
                         "on hardware at ~11 % of RspStart, which is itself "
                         "~21 % of non-idle work.")
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--audio-s8-tables",
                    default=(os.environ.get("DC_AUDIO_S8_TABLES")
                             or ("tools/dcaudio/audiorom-s8.tables.json"
                                 if os.environ.get("DC_AUDIO_S8") == "1"
                                 else "")),
                    help="W1. Path to the JSON tools/dcaudio/s8bank.py emits. "
                         "When set, audioheaders.c's wave-bank and extent "
                         "tables are repointed at an 8-bit-PCM audiorom.img, "
                         "which stops rspsim's ADPCM decoder running (~29 %% of "
                         "RspStart, itself 18.9 %% of busy CPU on silicon). "
                         "Empty (the default) emits no rule at all -- "
                         "audioheaders.c is not swapped and the build is "
                         "byte-identical. Set by DC_AUDIO_S8=1. THE IMAGE MUST "
                         "MATCH: dc_audio.c checks the file size at boot.")
    ap.add_argument("--audio", type=int, default=0, choices=(0, 1),
                    help="the build's DC_AUDIO. 0 (the default, matching "
                         "dc/Makefile) enables the S8 jaudio pool shrinks; 1 "
                         "leaves those pools at full size. The generated TUs "
                         "#error if this disagrees with the build.")
    ap.add_argument("--bgtex-demand", type=int,
                    default=int(os.environ.get("DC_BGTEX_DEMAND", "1") or "1"),
                    choices=(0, 1),
                    help="R1 (S10). 1 (the default, matching dc/Makefile) "
                         "rewrites mFM_LoadBGCommonTex()'s copy loop to read "
                         "the acre ground textures off the disc. 0 leaves the "
                         "vendored bcopy. It MUST match the --bgtex-demand= "
                         "that make_stub_data.py was run with, and "
                         "dc/Makefile's DC_BGTEX_DEMAND; the generated TU "
                         "#errors on either mismatch.")
    ap.add_argument("--npctex-pool", type=int,
                    default=int(os.environ.get("DC_NPCTEX_POOL", "1") or "1"),
                    choices=(0, 1),
                    help="R2 (S11). 1 (the default, matching dc/Makefile) "
                         "inserts the villager texture pool's load call into "
                         "aNPC_dma_draw_data_proc and its reset call into the "
                         "two mNpc_SetNpcList() sites that rebuild the whole "
                         "npclist. 0 inserts nothing. It MUST match the "
                         "--npctex-pool= that make_stub_data.py was run with, "
                         "and dc/Makefile's DC_NPCTEX_POOL; the generated TUs "
                         "#error on either mismatch.")
    ap.add_argument("--npcmdl-pool", type=int,
                    default=int(os.environ.get("DC_NPCMDL_POOL", "1") or "1"),
                    choices=(0, 1),
                    help="R3 (S12). 1 (the default, matching dc/Makefile) "
                         "inserts the villager MODEL pool's load call into "
                         "aNPC_dma_draw_data_proc beside R2's and its reset "
                         "call into the same two mNpc_SetNpcList() sites. 0 "
                         "inserts nothing. It MUST match the --npcmdl-pool= "
                         "that make_stub_data.py was run with, and "
                         "dc/Makefile's DC_NPCMDL_POOL; the generated TUs "
                         "#error on either mismatch.")
    ap.add_argument("--npc-seed", type=int,
                    default=int(os.environ.get("DC_NPC_SEED", "0") or "0"),
                    choices=(0, 1, 2),
                    help="N2c. Nonzero injects a dc_npc_seed() call into "
                         "mSDI_StartInitAfter immediately BEFORE "
                         "mNpc_SetNpcList, so the villager roster is repaired "
                         "before the town list is built from it. 0 (the "
                         "default) injects nothing. Must match dc/Makefile's "
                         "DC_NPC_SEED; the generated TU #errors on a mismatch "
                         "in either direction, because dc_npc_seed() is "
                         "defined unconditionally and the mismatch would "
                         "otherwise be silent. 1 and 2 inject the same call "
                         "(the observe-only behaviour of 2 is chosen inside "
                         "dc/src/dc_npcseed.c, not here).")
    ap.add_argument("--npcdiag", type=int,
                    default=int(os.environ.get("DC_NPCDIAG", "0") or "0"),
                    choices=(0, 1),
                    help="N3, the villager-construction diagnostic. 1 wraps the "
                         "nine serial gates between the npclist and a live "
                         "ACTOR in dc_npcdiag_gate() and adds the per-tick wade "
                         "sample, the cloth-bank tally and the clip one-shot. 0 "
                         "(the default) injects nothing and adds no file to "
                         "shrink.list -- a byte-identical revert. Must match "
                         "dc/Makefile's DC_NPCDIAG; ac_npc_ctrl.c_inc carries an "
                         "#error for a mismatch IN EITHER DIRECTION, because a "
                         "tree/build mismatch here prints a full diagnostic line "
                         "of zeroes and zero is a meaningful reading.")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    audio = bool(args.audio)
    bgtex = bool(args.bgtex_demand)
    npctex = bool(args.npctex_pool)
    npcmdl = bool(args.npcmdl_pool)
    npcseed = bool(args.npc_seed)
    npcdiag = bool(args.npcdiag)
    # S7 and S1c are built here, not at import time, because S10 rides in S7's
    # src/game/m_field_make.c entry and S11 + S12 + N3 ride in S1c's
    # src/actor/npc/ac_npc_ctrl.c_inc entry, and all need the parsed flags.
    rules_all = (RULES + [_s1c_rules(npctex, npcmdl, npcdiag)]
                 + _s11_reset_rules(npctex, npcmdl, npcseed)
                 + _s7_rules(bgtex) + _audio_rules(audio)
                 + _rspsim_rules(bool(args.rspsim_nofp))
                 + _audio_s8_rules(_load_s8_tables(args.audio_s8_tables)))
    # N3's other two TUs exist ONLY at --npcdiag=1. That is what keeps 0 a
    # byte-identical revert: no extra entry in shrink.list, no extra shadow on
    # the include path. The guard for the mismatched-tree case lives in
    # ac_npc_ctrl.c_inc above, which is in the tree at every setting.
    if npcdiag:
        rules_all += [_npcdiag_mgr_rules(), _npcdiag_cloth_rules()]
    expected = EXPECTED + ([] if audio else EXPECTED_AUDIO_OFF)

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

    # One output file per path, so a path claimed twice would be written twice
    # and only the last write would survive — silently dropping a whole rule.
    # dvdthread.c is the live instance (S9b + S8c), which is why _audio_rules()
    # merges those into a single entry; this is the check that keeps it merged.
    seen = {}
    for rel, _kind, _r in rules_all:
        if rel in seen:
            raise SystemExit(
                "make_src_shrink: %s appears in TWO rule entries. dc/Makefile\n"
                "  compiles one scratch copy per TU, so the second write would\n"
                "  overwrite the first and one rewrite would vanish with no\n"
                "  error. Merge them into a single entry." % rel)
        seen[rel] = 1

    for rel, kind, rules in rules_all:
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
        before = sum(b for _, b, _ in expected)
        after = sum(a for _, _, a in expected)
        print("== DC_SRC_SHRINK tree ==")
        print("  out            : {}".format(out_root))
        print("  DC_AUDIO       : {}   [{}]".format(
            args.audio,
            "S8 jaudio pools SHRUNK — this tree must not be built with "
            "DC_AUDIO=1 (the TUs #error)" if not audio
            else "S8 skipped; jaudio pools left at full size"))
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
        # Called out separately because it is the half that a DC_AUDIO=1 build
        # gives straight back, and because S9a's honest contribution is zero.
        s9b = sum(a - b for n, b, a in EXPECTED if n == "dvd_buf")
        s8 = sum(a - b for _, b, a in EXPECTED_AUDIO_OFF) if not audio else 0
        print("                   of which S9 {:+,} B (unconditional; S9a's"
              " dmabuffer is already".format(s9b))
        print("                   gc-sectioned away, so it counts 0) and S8"
              " {:+,} B [DC_AUDIO={}]".format(s8, args.audio))
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
        # S10 saves no .bss in THIS file — it removes 96 arrays from the KEEP
        # LIST, so the number lands in make_stub_data.py's report, not here.
        print("  R1 / S10       : mFM_LoadBGCommonTex copy loop {}"
              "   [--bgtex-demand={}]".format(
                  "-> dc_bgtex_load()" if bgtex else "left as bcopy",
                  args.bgtex_demand))
        if bgtex:
            print("                   150,880 B of mFM_grd_* no longer needs "
                  "keeping (80,736 B of it was)")
        # Same story as S10: R2's saving is a KEEP-LIST saving, so the number
        # lands in make_stub_data.py's report and not here.
        print("  R2 / S11       : aNPC_dma_draw_data_proc {}"
              "   [--npctex-pool={}]".format(
                  "+ dc_npctex_ensure(), 2 reset sites" if npctex
                  else "left alone", args.npctex_pool))
        if npctex:
            print("                   993,984 B of villager tex no longer needs "
                  "keeping (90,464 B of it was)")
        # ⚠️ R3 is the one rule in this file that SPENDS .bss. Printed as a cost
        # so nobody reads it off the S8/S7 lines above as another saving.
        print("  R3 / S12       : aNPC_dma_draw_data_proc {}"
              "   [--npcmdl-pool={}]".format(
                  "+ dc_npcmdl_ensure(), 2 reset sites" if npcmdl
                  else "left alone", args.npcmdl_pool))
        if npcmdl:
            print("                   194,400 B of villager mdl served from "
                  "16 x 7,552 B slots;")
            print("                   only 5,536 B (cbr_1) was ever resident, "
                  "so this COSTS ~115,296 B")
            print("                   of .bss and buys 31 species their "
                  "geometry")
        # N3 moves no bytes at all — it is an instrument. Printed so a build
        # log records whether the diagnostic seams are in the tree, which is
        # the one fact a [DC/NPCDIAG] line full of zeroes cannot tell you.
        print("  N3 / NPCDIAG   : {}   [--npcdiag={}]".format(
            "9 gates + wade sample + cloth tally instrumented (3 TUs)"
            if npcdiag else "guard only, nothing injected", args.npcdiag))
        if npcdiag:
            print("                   {} gate slots parsed from {}".format(
                len(_npcdiag_slots()), NPCDIAG_HDR))
        print("  (dc/Makefile adds -16,384 more by dropping KOS's dcache")
        print("   walk buffer out of dc/src/dc_os.c — see DC_OS_TU_OPT there.)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
