# =============================================================================
# opt-lists.mk — WHICH decomp TUs get -O3, and which get quarantined at -O0
# =============================================================================
# Included by dc/Makefile. Read the DC_OPT_PROFILE block there first.
#
# Two lists, and they are NOT symmetric:
#
#   OPT_HOT_SRC        gets $(DECOMP_HOT_OPT) — -O3 in the `perf` profile.
#                      This list is a COST: -O3 is bigger than -Os, and .text
#                      is RAM. An entry has to be on the per-frame path with
#                      EVIDENCE, not with an argument that it sounds hot.
#
#   OPT_QUARANTINE_SRC gets -O0 no matter what the profile asks for, because
#                      the TU is MEASURED to miscompile when optimized. An
#                      entry needs: the symptom, the run that showed it, and
#                      the date. "Looks risky" is not an entry — put it on the
#                      hot list and let the screenshot pair decide.
#
# PATHS ARE PRE-REWRITE, i.e. exactly as they appear under src/ or pc/src/.
# dc/Makefile runs each one through $(stubify)/$(shrinkify) itself, so an entry
# keeps working when a rewriter starts emitting that file. It also HARD-ERRORS
# on an entry that matches no TU in the build — a silently-inert list entry is
# the failure mode that let G1 sit unrun for two sessions (kb/RESUME.md).
#
# dc/src/* is NOT here. That is our own code and it is already at $(DC_OPT).
# =============================================================================

# -----------------------------------------------------------------------------
# THE HOT LIST
# -----------------------------------------------------------------------------
# Evidence is the G1 per-opcode histogram (kb/RESUME.md §1, 2026-08-05) and the
# phase split in kb/perf-dc.md §2, both measured in the town:
#
#   [EMU64H] tot=42.86ms | TRIN_INDEPEND 22.25/146  VTX 5.40/149  MTX 2.18/113
#   [PHASE]  draw=78.3  xform=12.2  cull=2.2
#
# 42.86 ms of a 78.3 ms frame is emu64 dispatch, and every handler in it is one
# TU: emu64.c. That single file is the whole reason this list exists.
# ⚠️ AND THE SHAPE OF THAT EVIDENCE BOUNDS THE WHOLE LIST. The frameskipped
# tick runs ALL of game_main and skips only the draw (graph.c:265-277), and it
# costs 7.3 ms against the drawn tick's 91.4. So emu64.c is ~58 ms on its own
# and EVERY OTHER src/ TU IN THIS PORT SHARES 7.3 ms. A perfect 2x on all of
# them together is ~+0.4 FPS. This is a one-TU problem with a long tail — which
# is why tier 2 below is deliberately short and skips the two biggest .text
# objects in the image.
OPT_HOT_SRC := \
	src/static/libforest/emu64/emu64.c \
	src/system/sys_matrix.c \
	src/system/sys_math.c \
	src/system/sys_math3d.c \
	src/game/m_skin_matrix.c \
	src/game/m_lights.c \
	src/game/m_actor.c \
	src/game/m_play.c \
	src/actor/ac_field_draw.c \
	src/game/m_field_info.c \
	src/game/m_lib.c \
	src/gfxalloc.c \
	src/graph.c \
	src/game.c \
	src/static/jaudio_NES/internal/rspsim.c \
	src/static/jaudio_NES/internal/driver.c \
	src/static/jaudio_NES/internal/system.c \
	src/static/jaudio_NES/internal/aictrl.c

# TIER 1 — measured
#   emu64.c              THE hot file: ~58 ms of a ~100 ms frame. Holds every
#                        opcode handler — dl_G_TRIN :4802-4940 is 22.25 ms over
#                        146 calls, dl_G_VTX :4618 is 5.40, dl_G_MTX :4471 is
#                        2.18 — plus set_position :2700-2805, which runs per
#                        emitted vertex. It also textually #includes
#                        emu64_utility.c (:92) and emu64_print.cpp (:308),
#                        neither of which is compiled standalone, so this one
#                        entry covers all three files.
#                        ⚠️ TWO REASONS IT IS ALSO THE FIRST QUARANTINE
#                        CANDIDATE: it is compiled as C++ (CXXFROMC_SRC), where
#                        falling off the end of a non-void function is UB that
#                        G++ turns into __builtin_unreachable and DELETES the
#                        path; and it type-puns hard on the per-vertex path
#                        (:4715 reads a u8[4] as u32; :890-916 does unaligned
#                        u32 access through u8*, which TRAPS on SH-4). The
#                        aliasing half is covered by UB_GUARDS, the alignment
#                        half is not. If an optimized image dies in the display
#                        list, start here.
#                        ⚠️ -O2 on this exact TU was device-verified on the
#                        armhf port ("crashes=0"; the armhf kb/perf.md was
#                        deleted in the 2026-08-09 audit — see git history).
#                        -O3 on it has never been tried anywhere in this
#                        port's history.
#
# TIER 2 — traced per frame in source, NOT individually measured. All of these
# share the 7.3 ms, so each is a small bet; they are here because they are
# small files of pure per-frame math, i.e. the best ratio of win to .text.
#   sys_matrix.c         the Matrix_* stack. Matrix_softcv3_load + Matrix_scale
#                        per drawn actor (m_actor.c:259,261); _MtxF_to_Mtx
#                        :533-561 converts float -> N64 s16.16 for every matrix
#                        handed to emu64. ⚠️ :534 and :634 pun an s32[4][4]
#                        through a u16* — the highest-risk non-emu64 site in
#                        the image, and it runs per actor per frame.
#   sys_math.c           sins/coss tables + the RNG the town seeds off (:7).
#   sys_math3d.c         collision/intersection math on the actor path.
#   m_skin_matrix.c      Skin_Matrix_PrjMulVector once per actor per frame
#                        (m_actor.c:550). 278 lines of float math — best
#                        effort/byte in tier 2.
#   m_lights.c           four light calls per drawn actor (m_actor.c:250-256);
#                        94 % of town vertices are lit (kb/perf-dc.md §3.2).
#   m_actor.c            Actor_info_call_actor :466 (every actor's mv_proc) and
#                        Actor_info_draw_actor :538 (projection, cull, draw).
#   m_play.c             the per-frame scene dispatch: play_main :873,
#                        Game_play_move :563, Game_play_draw :825,
#                        setupViewMatrix :646.
#   ac_field_draw.c      the acre emitter — walks the 3x3 block table
#                        (:203-233) and is where most of cmds=3458 is born.
#   m_field_info.c       mFI_FieldMove per frame; mFI_BGDispMake :1001 rebuilds
#                        the visible-block list.
#   m_lib.c              chase/angle/lerp helpers called from nearly every
#                        actor mv_proc. Diffuse, but tiny.
#
# TIER 1 (audio) — ADDED 2026-08-06, and unlike everything above these four were
# added to fix a MEASURED STUTTER rather than to chase a mean.
#   THE MEASUREMENT. With DC_AUDIO=1 the town hitches ~2x/second on real
#   hardware, and it reproduces: 192 [STUTTER] events / 420 s with audio vs 14
#   silent. A per-tick `snd=` counter (dc_vi.c) attributed it — and then a
#   DC_AUDIO_MAX_FRAMES=0 run, which keeps snd_stream_poll and the whole
#   KOS DMA/semaphore path live while doing NO synthesis, settled it:
#
#       synthesis ON    192 events   snd=17-84 ms   sndf=2-4
#       synthesis OFF    13 events   snd=0.0-0.3ms  sndf=0   (silent baseline 14)
#
#   The poll path costs 0.1 ms. The tens of milliseconds are inside
#   pc_audio_process_frame(), i.e. jaudio itself — and jaudio was the one
#   per-frame hot tree still at -Os while every other one was at -O3.
#   ⚠️ THE COST IS A TAIL, NOT A MEAN, and that is why this is worth doing:
#   synth_us reports 3,144 us (an EWMA, dc_audio.c:983) against a true mean of
#   3,832 (us/600 / synth_frames), while a stuttering frame spends 44 ms on 4
#   frames. Judge this change on the [STUTTER] RATE, never on synth_us.
#   rspsim.c              the software RSP: the sample-mixing inner loops.
#   driver.c              per-voice update, the largest of the four.
#   system.c              Nas_StartDma / Nas_FastCopy / the wave-cache path —
#                         2,541 lines, and where a wave-cache miss is serviced.
#   aictrl.c              the DAC-frame driver (DAC_SIZE*2 at :292 is the
#                         17.49 ms figure the whole audio budget rests on).
#   gfxalloc.c           GRAPH_ALLOC/gfxopen/gfxclose — one allocation per actor
#                        matrix per frame. Tiny file, called constantly.
#   graph.c              the frame loop (:401-429 batches the logic ticks) and
#                        graph_task_set00 :127-163, the entry to the traversal.
#   game.c               game_main.
#
# DELIBERATELY NOT ON THE LIST, and each one is a judgement worth keeping:
#   m_player.c           114,350 B of .text — the largest object in the image.
#                        Per-frame, but -O3 on it is a big .text bill for a
#                        share of 7.3 ms. Revisit with a measurement.
#   m_collision_bg.c     33,410 B, 8th-largest. Same argument.
#   c_keyframe.c         skeletal animation — currently near-dead, because the
#                        port constructs ZERO villagers (kb/RESUME.md §4).
#   m_npc.c/m_npc_walk.c cold for the same reason. Revisit after N2b.
#   the jaudio_NES tree  ✅ PROMOTED TO TIER 1, 2026-08-06 — this WAS the audio
#                        work. See the tier-1 note above. The entry that used to
#                        sit here said "add them WITH the audio work, not before
#                        it", and that is exactly what happened; kept as a
#                        record that the deferral was correct rather than lucky.
#                        ⚠️ jammain_2.c is deliberately NOT promoted with them —
#                        it is the one C++ TU with a missing return AND 22
#                        uninitialised reads, i.e. the first quarantine suspect
#                        (see KNOWN CANDIDATES below). Promote it only after
#                        these four are proven, and separately, so a regression
#                        has one cause.
#   pc/src/*             none of the three DC-compiled files is per-frame.
#   src/static/dolphin/  the mtx tree is FILTERED OUT of this build entirely;
#                        the linked PSMTX* are dc/src/dc_mtx.c's.

# -----------------------------------------------------------------------------
# THE QUARANTINE LIST
# -----------------------------------------------------------------------------
# Empty as of 2026-08-06: the first optimized whole-tree builds (-Os, then -Os
# + -O3 hot list) both linked, booted, walked the town and printed
# `ASSET MISSING 0 / crashes=0`, so nothing has EARNED an entry yet. Do not
# pre-populate it out of caution — an unjustified entry is permanently slower
# code that nobody will ever dare delete.
#
# Format when you add one:
#	src/path/to/file.c \   # SYMPTOM, run dir, date
OPT_QUARANTINE_SRC :=

# -----------------------------------------------------------------------------
# KNOWN CANDIDATES — measured UB, NOT quarantined, and why not
# -----------------------------------------------------------------------------
# From `DC_TARGET=warnscan bash dc/build-dc.sh` on 2026-08-06, reduced with
# tools/dcopt/warnscan_report.py. 35 return-type warnings in 30 files, 99
# uninitialised-read warnings, 8 array-bounds, 3 sequence-point.
#
#   ✅ emu64.c — THE hot file, and it is CLEAN on return-type. That matters
#      more than any other single line of this scan: emu64.c is one of the four
#      TUs compiled as C++, where a missing return is UB that G++ turns into
#      __builtin_unreachable and deletes the path. It has none.
#
#   ⚠️ src/static/jaudio_NES/internal/jammain_2.c — the ONE file where both
#      halves of that hazard meet: it is in CXXFROMC_SRC (compiled as C++) AND
#      it has a missing return, plus 22 uninitialised-read warnings, the most
#      in the tree. It is NOT quarantined because at DC_AUDIO=0 (the default)
#      the sequencer never ticks, so quarantining it would buy nothing and
#      prove nothing. ⚠️ WHEN THE AUDIO WORK STARTS, THIS IS THE FIRST FILE TO
#      SUSPECT — build DC_AUDIO=1 twice, once with
#      DC_OPT_O0_EXTRA=src/static/jaudio_NES/internal/jammain_2.c, and compare.
#
#   The other 28 files are ordinary C, where a missing return costs the caller
#   a garbage return value rather than a deleted code path, and the port
#   demonstrably walks the town with all 35 outstanding. They are the ranked
#   candidate list for a bisect, not a defect list:
#      python3 tools/dcopt/warnscan_report.py dc/build/warnscan.log --paths

# DC_OPT_O0_EXTRA is the bisecting knob: a space-separated list of the same
# kind of paths, from the environment, appended to the quarantine list without
# editing this file.
#
#   DC_OPT_O0_EXTRA='src/a.c src/b.c' bash dc/build-dc.sh
#
# tools/dcopt/bisect_o0.sh drives it. When a bisect finds the file, it moves
# into OPT_QUARANTINE_SRC above WITH ITS EVIDENCE — the env knob is for the
# search, not for the fix.
OPT_QUARANTINE_SRC += $(DC_OPT_O0_EXTRA)
