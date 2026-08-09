#!/usr/bin/env python3
"""
run_report.py — turn a harness console.log into a one-screen regression report,
and diff two of them.

WHY
---
A smoke run of the game always exits 1 (`kb/traps.md`: the game never returns,
so the end-marker checks can never pass), so the exit code carries no signal and
every judgement about a build has been made by grepping the same eight patterns
out of a 2.6 MB log by hand. That is slow, it is not diffable, and it has
already produced two wrong "regression" calls — a short run that was the human
closing the emulator, and a screenshot run read as a progression run.

This reduces a console.log to the numbers that actually decide "did this build
get worse", and `--vs` prints the two side by side with the direction of each
change marked. It is deliberately dumb: it parses, it does not judge, except for
the handful of counters that have a known-correct value (`LOST=0`, `ptdrop=0`,
`zero=0`, `over=0`, `vidbad=0`, no exceptions).

⚠️ A field a build does not PRINT summarises to None and prints as
`not measured` — never as 0. A gate cannot pass on a counter that was never
compiled in, and `vidbad=0` over `vidchk=0` checks is an unrun gate, not a pass.

USAGE
-----
    python3 tools/dcqa/run_report.py <run>/console.log
    python3 tools/dcqa/run_report.py <new>/console.log --vs <old>/console.log
    python3 tools/dcqa/run_report.py <run>/console.log --json

⚠️ Only compare like with like. A `DC_FB_IMAGE` screenshot run reaches roughly
a tenth of the frames of a plain run because it streams ~205 KB of base64 per
probe over a 57600-baud SCIF — diffing one against a progression run reads as a
catastrophic regression. `--vs` warns when one side has framebuffer dumps and
the other does not.

No third-party modules.
"""

import argparse
import json
import re
import sys
from collections import Counter

# ---------------------------------------------------------------- parsing ---

RE_PERF = re.compile(
    r"\[PERF\]\s+([\d.]+) FPS \| (\d+)% speed \| draws=(\d+).*?cmds=(\d+) "
    r"crashes=(\d+) gx=([\d.]+)ms"
)
RE_SCENE = re.compile(r"\[SCENE_MODE\]\s+(\d+)\s*->\s*(\d+)")
RE_PVR_FRAMES = re.compile(
    r"\[DC/PVR\] frames=(\d+) batches=(\d+) tris in=(\d+) out=(\d+) "
    r"clipped=(\d+) dropped=(\d+) unsupported_prims=(\d+)"
)
RE_PVR_PT = re.compile(r"\[DC/PVR\] pt batches=(\d+) verts=(\d+).*?pthi=(\d+)/(\d+) ptdrop=(\d+)")
RE_TEX = re.compile(
    r"\[DC/TEX\] uploads=(\d+) hits=(\d+) resident=(\d+) B evictions=(\d+) rejects=(\d+)"
)
RE_TEXLOG = re.compile(r"\[DC/TEXLOG\] (\d+)x(\d+) .*?nonzero=(\d+)/(\d+)")
RE_ARAM = re.compile(
    r"\[DC/ARAM\] LRU w:mapped=(\d+) stored=(\d+) audio=(\d+) LOST=(\d+) \| "
    r"r:cache=(\d+) disc=(\d+) zero=(\d+)"
)
RE_FBNONZERO = re.compile(r"FBNONZERO (\d+) of (\d+)")
RE_FBIMG = re.compile(r"FBIMG BEGIN (\d+) (\d+) \S+ frame=(\d+)")
RE_TAG = re.compile(r"^\[([A-Z0-9_/]+)\]")

# G3's cull counters, plus the vertex-index side channel bolted onto them.
# The line GROWS with the build: `vid=`/`over=`/`pdec=`/`ptgen=`/`pmix=` are all
# absent under -DDC_GX_NO_VTXID, and `falsecull=`/`gfxp_bad=`/`nocmp=` only
# exist under -DDC_EMU64_CULL_VERIFY. So the fixed prefix is matched on its own
# and every later field is picked up by an optional key=value sweep -- an absent
# field must summarise to None ("not measured"), NEVER to 0, or a build that
# cannot even count them reads as a clean gate pass.
RE_EMU64C = re.compile(
    r"\[EMU64C\]\s+trin=(\d+) cull=(\d+) vis=(\d+) punt=(\d+) refs=(\d+)"
)
RE_EMU64C_VID = re.compile(r"\bvid=(\d+)/(\d+)")
RE_EMU64C_KV = re.compile(
    r"\b(over|pdec|ptgen|pmix|reinst|falsecull|gfxp_bad|nocmp)=(\d+)"
)

# -DDC_GX_VTXID_VERIFY only. vidbad must be 0 -- but vidbad=0 with vidchk=0 is
# an unrun gate, not a pass, and the report says so out loud.
RE_VTXID = re.compile(r"\[DC/PVR\] vtxid vidchk=(\d+) vidbad=(\d+)")

# ⚠️ MEASUREMENT RULE 10 (kb/RESUME.md): THE SEVEN BUCKETS DO NOT SHARE A
# DENOMINATOR. `memo` is charged per VERTEX, `emit` per PRIMITIVE, and the
# middle five (xf/lit/tex/shade/post) are charged only on a memo MISS. So:
#   - never sum them against `v` or against a per-vertex cost;
#   - never compare bucket-for-bucket across two runs whose memo hit rate
#     differs without saying so -- the middle five move purely because fewer or
#     more vertices missed, with no code change at all.
# That is why the vsplit_* metrics below are DIAGNOSTIC (they never flip the
# verdict) and why the diff prints a memo-mix warning when the rates diverge.
RE_VTXSPLIT = re.compile(
    r"\[VTXSPLIT\]\s+memo=([\d.]+) xf=([\d.]+) lit=([\d.]+) tex=([\d.]+) "
    r"shade=([\d.]+) post=([\d.]+) emit=([\d.]+)\s+\|\s+sum=([\d.]+)"
)
RE_VTXSPLIT_KV = re.compile(r"\b(prims|samp|memohit|drops)=(\d+)")

# Lines that mean the run went wrong, not merely that it was noisy.
BAD_MARKERS = [
    ("exception", re.compile(r"\[DC/EXC\].*(unhandled|fault|PC=)", re.I)),
    ("assert", re.compile(r"ASSERTION FAILURE|Assertion \".*\" failed")),
    ("oom", re.compile(r"Out of memory|sbrk_base|\bOOM\b")),
    ("short_read", re.compile(r"SHORT READ")),
    ("aram_lost", re.compile(r"LOST=[1-9]")),
    # The cheapest asset-side health check there is. A stubbed asset that never
    # loads decodes to a transparent rectangle and reads exactly like a
    # renderer bug -- that is how the reply box was misdiagnosed for two days
    # (kb/traps.md). This line is the difference between "the renderer is
    # wrong" and "the image does not contain the thing".
    ("asset_missing", re.compile(r"ASSET MISSING")),
]

# Absolute tolerance for small-integer counters, where a relative band is
# useless because the baseline is 0 or near it.
TOLERANCE_ABS = {
    "texlog_blank": 3,
    "tex_uploads": 5,
}


def parse(path):
    r = {
        "path": str(path),
        "lines": 0,
        "perf": [],
        "scene_edges": [],
        "scene_modes": set(),
        "pvr": None,
        "pt": None,
        "tex": None,
        "texlog": {"total": 0, "blank": 0, "partial": 0},
        "aram": None,
        "emu64c": None,
        "vtxid": None,
        "vtxsplit": None,
        "fb": {"probes": 0, "nonzero_max": 0, "pixels": 0, "images": []},
        "tags": Counter(),
        "bad": Counter(),
        "top_repeats": [],
    }
    repeat = Counter()

    with open(path, "r", errors="replace") as fh:
        for line in fh:
            r["lines"] += 1
            line = line.rstrip("\n")

            # FBROW carries base64 pixel data, which matches almost any text
            # pattern by accident — `OOM` and `LOST=1` both occur in real
            # screenshots. Never let it reach the matchers.
            if line.startswith("FBROW"):
                continue

            m = RE_TAG.match(line)
            if m:
                r["tags"][m.group(1)] += 1
            else:
                # untagged spam (emu64's Printf0 etc.) — worth counting
                repeat[line[:100]] += 1

            m = RE_PERF.search(line)
            if m:
                r["perf"].append(
                    {
                        "fps": float(m.group(1)),
                        "speed": int(m.group(2)),
                        "draws": int(m.group(3)),
                        "cmds": int(m.group(4)),
                        "crashes": int(m.group(5)),
                        "gx_ms": float(m.group(6)),
                    }
                )
                continue

            m = RE_SCENE.search(line)
            if m:
                r["scene_edges"].append((int(m.group(1)), int(m.group(2))))
                r["scene_modes"].add(int(m.group(1)))
                r["scene_modes"].add(int(m.group(2)))
                continue

            m = RE_PVR_FRAMES.search(line)
            if m:
                r["pvr"] = {
                    "frames": int(m.group(1)),
                    "batches": int(m.group(2)),
                    "tris_in": int(m.group(3)),
                    "tris_out": int(m.group(4)),
                    "clipped": int(m.group(5)),
                    "dropped": int(m.group(6)),
                    "unsupported": int(m.group(7)),
                }
                continue

            m = RE_PVR_PT.search(line)
            if m:
                r["pt"] = {
                    "batches": int(m.group(1)),
                    "verts": int(m.group(2)),
                    "hi": int(m.group(3)),
                    "cap": int(m.group(4)),
                    "drop": int(m.group(5)),
                }
                continue

            m = RE_TEX.search(line)
            if m:
                r["tex"] = {
                    "uploads": int(m.group(1)),
                    "hits": int(m.group(2)),
                    "resident": int(m.group(3)),
                    "evictions": int(m.group(4)),
                    "rejects": int(m.group(5)),
                }
                continue

            m = RE_TEXLOG.search(line)
            if m:
                nz, tot = int(m.group(3)), int(m.group(4))
                r["texlog"]["total"] += 1
                if nz == 0:
                    r["texlog"]["blank"] += 1
                elif nz < tot // 2:
                    r["texlog"]["partial"] += 1
                continue

            m = RE_ARAM.search(line)
            if m:
                r["aram"] = {
                    "mapped": int(m.group(1)),
                    "stored": int(m.group(2)),
                    "audio": int(m.group(3)),
                    "lost": int(m.group(4)),
                    "cache": int(m.group(5)),
                    "disc": int(m.group(6)),
                    "zero": int(m.group(7)),
                }
                continue

            m = RE_EMU64C.search(line)
            if m:
                e = {
                    "trin": int(m.group(1)),
                    "cull": int(m.group(2)),
                    "vis": int(m.group(3)),
                    "punt": int(m.group(4)),
                    "refs": int(m.group(5)),
                }
                v = RE_EMU64C_VID.search(line)
                if v:
                    # A = TRIN batches that ARMED the side channel,
                    # B = vertex references handed to it. Both are REACH.
                    e["vid_batches"] = int(v.group(1))
                    e["vid_refs"] = int(v.group(2))
                for k, val in RE_EMU64C_KV.findall(line):
                    e[k] = int(val)
                r["emu64c"] = e
                continue

            m = RE_VTXID.search(line)
            if m:
                r["vtxid"] = {"chk": int(m.group(1)), "bad": int(m.group(2))}
                continue

            m = RE_VTXSPLIT.search(line)
            if m:
                names = ("memo", "xf", "lit", "tex", "shade", "post", "emit", "sum")
                x = {n: float(m.group(i + 1)) for i, n in enumerate(names)}
                for k, val in RE_VTXSPLIT_KV.findall(line):
                    x[k] = int(val)
                r["vtxsplit"] = x
                continue

            m = RE_FBNONZERO.search(line)
            if m:
                r["fb"]["probes"] += 1
                r["fb"]["nonzero_max"] = max(r["fb"]["nonzero_max"], int(m.group(1)))
                r["fb"]["pixels"] = int(m.group(2))
                continue

            m = RE_FBIMG.search(line)
            if m:
                r["fb"]["images"].append(int(m.group(3)))
                continue

            for name, rx in BAD_MARKERS:
                if rx.search(line):
                    r["bad"][name] += 1

    r["scene_modes"] = sorted(r["scene_modes"])
    r["top_repeats"] = [(c, t) for t, c in repeat.most_common(5) if c > 20]
    return r


# --------------------------------------------------------------- reporting ---


def _fps_stats(perf):
    if not perf:
        return None
    f = sorted(p["fps"] for p in perf)
    n = len(f)
    return {
        "samples": n,
        "min": f[0],
        "p50": f[n // 2],
        "max": f[-1],
        "mean": round(sum(f) / n, 2),
    }


def summarise(r):
    """Reduce a parsed run to the ~15 numbers a regression call is made on."""
    fps = _fps_stats(r["perf"])
    e = r["emu64c"] or {}
    x = r["vtxsplit"] or {}
    s = {
        "lines": r["lines"],
        "frames": (r["pvr"] or {}).get("frames"),
        "scene_modes": r["scene_modes"],
        "deepest_scene": max(r["scene_modes"]) if r["scene_modes"] else None,
        "fps_min": fps and fps["min"],
        "fps_p50": fps and fps["p50"],
        "fps_max": fps and fps["max"],
        "draws_max": max((p["draws"] for p in r["perf"]), default=None),
        "batches": (r["pvr"] or {}).get("batches"),
        "tris_out": (r["pvr"] or {}).get("tris_out"),
        "pvr_dropped": (r["pvr"] or {}).get("dropped"),
        "pvr_unsupported": (r["pvr"] or {}).get("unsupported"),
        "pt_drop": (r["pt"] or {}).get("drop"),
        "pt_hi": (r["pt"] or {}).get("hi"),
        "tex_uploads": (r["tex"] or {}).get("uploads"),
        "tex_resident": (r["tex"] or {}).get("resident"),
        "tex_rejects": (r["tex"] or {}).get("rejects"),
        "texlog_total": r["texlog"]["total"] or None,
        "texlog_blank": r["texlog"]["blank"] if r["texlog"]["total"] else None,
        "aram_mapped": (r["aram"] or {}).get("mapped"),
        "aram_lost": (r["aram"] or {}).get("lost"),
        "aram_zero": (r["aram"] or {}).get("zero"),
        "fb_probes": r["fb"]["probes"] or None,
        "fb_images": len(r["fb"]["images"]) or None,
        "fb_nonzero_max": r["fb"]["nonzero_max"] or None,
        # G3 cull + the vertex-index side channel. `.get()` throughout, so a
        # field the build never printed stays None = NOT MEASURED.
        "emu_trin": e.get("trin"),
        "emu_cull": e.get("cull"),
        "emu_vis": e.get("vis"),
        "emu_punt": e.get("punt"),
        "emu_refs": e.get("refs"),
        "emu_vid_batches": e.get("vid_batches"),
        "emu_vid_refs": e.get("vid_refs"),
        "emu_over": e.get("over"),
        "emu_pdec": e.get("pdec"),
        "emu_ptgen": e.get("ptgen"),
        "emu_pmix": e.get("pmix"),
        "emu_reinst": e.get("reinst"),
        "emu_falsecull": e.get("falsecull"),
        "emu_gfxp_bad": e.get("gfxp_bad"),
        "emu_nocmp": e.get("nocmp"),
        "vtxid_chk": (r["vtxid"] or {}).get("chk"),
        "vtxid_bad": (r["vtxid"] or {}).get("bad"),
        # ⚠️ rule 10 applies to every vsplit_* below: no shared denominator.
        "vsplit_memo": x.get("memo"),
        "vsplit_xf": x.get("xf"),
        "vsplit_lit": x.get("lit"),
        "vsplit_tex": x.get("tex"),
        "vsplit_shade": x.get("shade"),
        "vsplit_post": x.get("post"),
        "vsplit_emit": x.get("emit"),
        "vsplit_sum": x.get("sum"),
        "vsplit_prims": x.get("prims"),
        "vsplit_memohit": x.get("memohit"),
        "vsplit_drops": x.get("drops"),
        "bad": dict(r["bad"]),
    }
    return s


def _memo_mix(s):
    """memohit per primitive -- NOT a hit RATE (the denominators differ, rule
    10). It exists only to answer "did the memo mix move between these two
    runs", which decides whether the middle five buckets may be compared at
    all. Returns None when either half was not measured."""
    prims, hit = s.get("vsplit_prims"), s.get("vsplit_memohit")
    if not prims or hit is None:
        return None
    return hit / prims


# Relative tolerance, per metric, below which a change is NOISE and not a
# verdict. Two runs of the SAME build do not agree: the autostart presses land
# at different points, the camera ends up somewhere else, and the host is
# running a second emulator half the time. Measured spread on identical builds
# is well under 1 % on frames and ~0.1 FPS on the percentiles, so 3 % is loose
# enough never to fire on noise and tight enough to catch a real stall.
#
# Counters in MUST_BE_ZERO get no tolerance at all — 1 is not 0.
TOLERANCE = {
    # ⚠️ TOTAL FRAMES IS CONFOUNDED BY SCENE MIX, so its band is wide on
    # purpose. The town runs at ~11 FPS and the train intro at ~30, so a run
    # that reaches the town SOONER accumulates FEWER frames over the same 600 s
    # -- an improvement reads as a 25 % "regression". Two runs of one build have
    # been measured 10,499 vs 7,979 for exactly that reason. Use it to catch a
    # hang or a crash loop, never as a progression metric; `deepest_scene` is
    # the progression metric.
    "frames": 0.30,
    "fps_min": 0.10,   # the minimum is a single worst frame; it is the noisiest
    "fps_p50": 0.10,
    "fps_max": 0.05,
    # Blank textures are scene-dependent: an object drawn in one run and not the
    # other changes this by one or two with no code change. A jump of more than
    # a handful is a real keep-list or loader problem.
    "texlog_blank": 3.0,
    # Side-channel reach follows the scene the same way `frames` does -- a run
    # that stands somewhere else arms a different number of batches.
    "emu_vid_batches": 0.10,
    "emu_vid_refs": 0.10,
    # Sampled millisecond buckets. 10 % is inside the jitter of a 1-in-16
    # sampler, and anything smaller than that is not a finding.
    "vsplit_memo": 0.10,
    "vsplit_xf": 0.10,
    "vsplit_lit": 0.10,
    "vsplit_tex": 0.10,
    "vsplit_shade": 0.10,
    "vsplit_post": 0.10,
    "vsplit_emit": 0.10,
    "vsplit_sum": 0.10,
}

# Which direction is an improvement. None = report the change, judge nothing.
DIRECTION = {
    "frames": +1,
    "deepest_scene": +1,
    "fps_min": +1,
    "fps_p50": +1,
    "fps_max": +1,
    "tris_out": None,
    "batches": None,
    "draws_max": None,
    "pvr_dropped": -1,
    "pvr_unsupported": -1,
    "pt_drop": -1,
    "pt_hi": None,
    "tex_uploads": None,
    "tex_resident": None,
    "tex_rejects": -1,
    "texlog_total": None,
    "texlog_blank": -1,
    "aram_mapped": None,
    "aram_lost": -1,
    "aram_zero": -1,
    "fb_nonzero_max": None,
    "lines": None,
    # --- G3 cull + vertex-index side channel -------------------------------
    "emu_trin": None,      # scene mix decides all four of these
    "emu_cull": None,
    "emu_vis": None,
    "emu_refs": None,
    "emu_punt": -1,        # a punted batch is one the side channel gave up on
    "emu_vid_batches": +1,  # REACH: worse when SMALLER
    "emu_vid_refs": +1,     # REACH: worse when SMALLER
    "emu_over": -1,         # must be 0: the reference list outgrew its bound
    "emu_pdec": -1,         # punt reason: decal Z
    "emu_ptgen": -1,        # punt reason: G_TEXTURE_GEN
    "emu_pmix": -1,         # punt reason: mixed MTX_NONSHARED
    "emu_reinst": -1,
    "emu_falsecull": -1,
    "emu_gfxp_bad": -1,
    "emu_nocmp": -1,
    "vtxid_chk": None,      # coverage, not quality -- see VTXID_COVERAGE below
    "vtxid_bad": -1,
    # --- xform split (rule 10: no shared denominator) ----------------------
    "vsplit_memo": -1,
    "vsplit_xf": -1,
    "vsplit_lit": -1,
    "vsplit_tex": -1,
    "vsplit_shade": -1,
    "vsplit_post": -1,
    "vsplit_emit": -1,
    "vsplit_sum": -1,
    "vsplit_prims": None,
    "vsplit_memohit": None,
    "vsplit_drops": None,
}

# Counters with one correct value, whatever the build. A non-zero here is a
# hard FAIL in the verdict -- never a percentage delta, and never excused by
# having gone DOWN (3 is not 0 either).
MUST_BE_ZERO = ["pvr_dropped", "pvr_unsupported", "pt_drop", "tex_rejects",
                "aram_lost", "aram_zero",
                "emu_over",        # the side channel silently went unarmed
                "emu_reinst", "emu_falsecull", "emu_gfxp_bad", "emu_nocmp",
                "vtxid_bad"]       # ⚠️ only meaningful when vtxid_chk > 0

# Metrics whose DIRECTION is worth printing but which must never flip the
# verdict on their own. The punt reasons are diagnostics -- they say WHY the
# side channel declined a batch, and a scene with more decals legitimately
# moves them. The vsplit_* buckets are worse: rule 10 means the middle five
# move with the memo hit rate alone, so a bucket-for-bucket "regression" is
# not a claim this script is entitled to make.
DIAGNOSTIC = {"emu_pdec", "emu_ptgen", "emu_pmix",
              "vsplit_memo", "vsplit_xf", "vsplit_lit", "vsplit_tex",
              "vsplit_shade", "vsplit_post", "vsplit_emit", "vsplit_sum"}


def _n(v):
    """Thousands-grouped, or `n/a` for a field the build never printed."""
    return "n/a" if v is None else f"{v:,}"


def _s(v):
    """A missing field is `not measured`, and must never read as 0."""
    if v is None:
        return "not measured"
    return f"{v:,}" if isinstance(v, (int, float)) and not isinstance(v, bool) \
        else str(v)


def print_report(r, fh=sys.stdout):
    s = summarise(r)
    p = lambda *a: print(*a, file=fh)

    p(f"RUN  {r['path']}")
    p(f"     {s['lines']:,} console lines")
    p("")
    p("PROGRESSION")
    p(f"  frames           {s['frames']}")
    p(f"  scene modes      {s['scene_modes']}  (edges: "
      f"{' -> '.join(str(a) for a, _ in r['scene_edges']) or '-'}"
      f"{' -> ' + str(r['scene_edges'][-1][1]) if r['scene_edges'] else ''})")
    if s["fps_p50"] is not None:
        p(f"  FPS              min {s['fps_min']}  p50 {s['fps_p50']}  max {s['fps_max']}")
    p("")
    p("RENDERER")
    p(f"  batches          {s['batches']}")
    p(f"  tris out         {s['tris_out']}")
    p(f"  draws (max)      {s['draws_max']}")
    p(f"  pt hi / drop     {s['pt_hi']} / {s['pt_drop']}")
    p(f"  dropped/unsup    {s['pvr_dropped']} / {s['pvr_unsupported']}")
    p("")
    p("TEXTURES")
    p(f"  uploads          {s['tex_uploads']}   resident {s['tex_resident']} B"
      f"   rejects {s['tex_rejects']}")
    if s["texlog_total"]:
        blank = s["texlog_blank"]
        pct = 100.0 * blank / s["texlog_total"]
        p(f"  DC_TEX_LOG       {s['texlog_total']} decoded, {blank} BLANK ({pct:.1f} %),"
          f" {r['texlog']['partial']} <half-lit")
    else:
        p("  DC_TEX_LOG       (not built in)")
    p("")
    p("EMU64 CULL / VERTEX-ID")
    if s["emu_trin"] is None:
        p("  [EMU64C]         (not built in)")
    else:
        p(f"  trin/cull/vis    {s['emu_trin']} / {s['emu_cull']} / {s['emu_vis']}"
          f"   refs {s['emu_refs']}")
        if s["emu_vid_batches"] is None:
            p("  vid side chan    (not measured -- DC_GX_NO_VTXID build)")
        else:
            p(f"  vid armed        {s['emu_vid_batches']} batches"
              f" / {s['emu_vid_refs']} refs   (reach: bigger is better)")
            p(f"  over             {s['emu_over']}"
              f"   (must be 0 -- list outgrew its bound, batch went unarmed)")
        punts = [s["emu_pdec"], s["emu_ptgen"], s["emu_pmix"]]
        if all(v is None for v in punts):
            p(f"  punt             {s['emu_punt']}   (reasons not measured)")
        else:
            p(f"  punt             {s['emu_punt']}   pdec {s['emu_pdec']}"
              f"  ptgen {s['emu_ptgen']}  pmix {s['emu_pmix']}")
        ver = [s["emu_falsecull"], s["emu_gfxp_bad"], s["emu_nocmp"]]
        if all(v is None for v in ver):
            p(f"  reinst           {s['emu_reinst']}"
              f"   (CULL_VERIFY fields not built in)")
        else:
            p(f"  VERIFY           reinst {s['emu_reinst']}"
              f"  falsecull {s['emu_falsecull']}  gfxp_bad {s['emu_gfxp_bad']}"
              f"  nocmp {s['emu_nocmp']}")
    if s["vtxid_bad"] is None:
        p("  vtxid gate       (not built in -- needs -DDC_GX_VTXID_VERIFY)")
    elif not s["vtxid_chk"]:
        # A zero-coverage gate is NOT a pass. This is the whole point of
        # printing vidchk next to vidbad.
        p(f"  vtxid gate       vidbad {s['vtxid_bad']} but vidchk 0"
          f"  -> NOT RUN, proves nothing")
    else:
        verdict = "PASS" if s["vtxid_bad"] == 0 else "FAIL"
        p(f"  vtxid gate       {s['vtxid_chk']:,} checked,"
          f" {s['vtxid_bad']} bad  -> {verdict}")
    p("")
    p("XFORM SPLIT  ⚠️ buckets DO NOT share a denominator (rule 10):")
    p("             memo is per VERTEX, emit per PRIMITIVE, the middle five")
    p("             only on a memo MISS. Never sum them against v.")
    if s["vsplit_sum"] is None:
        p("  [VTXSPLIT]       (not built in -- needs -DDC_PVR_VTXSPLIT=<N>)")
    else:
        p(f"  memo {s['vsplit_memo']}  xf {s['vsplit_xf']}  lit {s['vsplit_lit']}"
          f"  tex {s['vsplit_tex']}  shade {s['vsplit_shade']}"
          f"  post {s['vsplit_post']}  emit {s['vsplit_emit']}  ms")
        mix = _memo_mix(s)
        p(f"  sum              {s['vsplit_sum']} ms   prims {_n(s['vsplit_prims'])}"
          f"   memohit {_n(s['vsplit_memohit'])}"
          f"{'' if mix is None else f' ({mix:.3f}/prim)'}"
          f"   drops {s['vsplit_drops']}")
    p("")
    p("ARAM / FB")
    p(f"  aram mapped      {s['aram_mapped']}   LOST {s['aram_lost']}   zero {s['aram_zero']}")
    if s["fb_probes"]:
        p(f"  fb probes        {s['fb_probes']}   images {s['fb_images']}"
          f"   max lit {s['fb_nonzero_max']}/{r['fb']['pixels']}")
    p("")

    flags = []
    for k in MUST_BE_ZERO:
        v = s.get(k)
        if v:
            flags.append(f"{k}={v} (must be 0)")
    # vidbad=0 over zero checks is an unrun gate. Reporting it as a pass is how
    # a verifier gets believed while never having executed -- say it instead.
    if s["vtxid_bad"] == 0 and not s["vtxid_chk"]:
        flags.append("vtxid gate did not run (vidchk=0) -- vidbad=0 is not a pass")
    for name, n in s["bad"].items():
        flags.append(f"{name} x{n}")
    if r["top_repeats"]:
        c, t = r["top_repeats"][0]
        if c > 500:
            flags.append(f"console flood: {c} x {t[:60]!r}")

    if flags:
        p("FLAGS")
        for f in flags:
            p(f"  !! {f}")
    else:
        p("FLAGS  none")
    return s


def print_diff(new, old, fh=sys.stdout):
    a, b = summarise(new), summarise(old)
    p = lambda *a_: print(*a_, file=fh)

    if bool(a.get("fb_images")) != bool(b.get("fb_images")):
        p("⚠️  one side is a DC_FB_IMAGE screenshot run and the other is not.")
        p("    Frame counts are NOT comparable (kb/RESUME.md §2).")
        p("")

    amix, bmix = _memo_mix(a), _memo_mix(b)
    if amix is not None and bmix is not None and abs(amix - bmix) > 0.10 * bmix:
        p(f"⚠️  memo mix moved ({bmix:.3f} -> {amix:.3f} hits/prim). The five")
        p("    memo-MISS buckets (xf/lit/tex/shade/post) move with the hit rate")
        p("    alone -- do NOT read them as a code change (rule 10).")
        p("")

    p(f"{'metric':<18} {'old':>14} {'new':>14}   delta")
    p("-" * 62)
    regressions = []
    for k, direction in DIRECTION.items():
        ov, nv = b.get(k), a.get(k)
        if ov is None and nv is None:
            continue
        if ov is None or nv is None:
            # NOT MEASURED on one side -- a field this build does not print, not
            # a zero. Say which side, and never derive a delta from it.
            side = "old" if ov is None else "new"
            note = f"(not measured on the {side} side)"
            if k in MUST_BE_ZERO and nv:
                note += "  !! FAIL (must be 0)"
                regressions.append(f"{k}={nv} (must be 0)")
            p(f"{k:<18} {_s(ov):>14} {_s(nv):>14}   {note}")
            continue
        d = nv - ov
        if isinstance(d, float):
            d = round(d, 3)
        mark = ""
        tol = TOLERANCE.get(k, 0.0)
        noise = (ov and abs(d) <= tol * abs(ov)) or \
                abs(d) <= TOLERANCE_ABS.get(k, 0)
        if k in MUST_BE_ZERO and nv:
            # Not a percentage: 3 is as wrong as 3000, and 5 -> 3 is not "ok".
            mark = "  !! FAIL (must be 0)"
            regressions.append(f"{k}={nv} (must be 0)")
        elif d and direction:
            good = (d > 0) == (direction > 0)
            if good:
                mark = "  ok"
            elif noise:
                mark = f"  ~noise (<={tol:.0%})"
            elif k in DIAGNOSTIC:
                mark = "  worse (diagnostic)"
            else:
                mark = "  REGRESSION"
                regressions.append(f"{k}: {ov} -> {nv}")
        elif d:
            mark = "  changed"
        p(f"{k:<18} {ov:>14,} {nv:>14,}   {d:+,}{mark}"
          if isinstance(ov, (int, float)) and isinstance(nv, (int, float))
          else f"{k:<18} {_s(ov):>14} {_s(nv):>14}   {mark}")

    p("")
    if a.get("vtxid_bad") == 0 and not a.get("vtxid_chk"):
        p("⚠️  vtxid_bad=0 with vidchk=0 on the new side: the vertex-id gate")
        p("    never ran. That is not a pass.")
    ns, os_ = a.get("scene_modes") or [], b.get("scene_modes") or []
    if ns != os_:
        p(f"scene modes        {os_} -> {ns}")
    newbad = {k: v for k, v in a["bad"].items() if v > b["bad"].get(k, 0)}
    if newbad:
        p(f"NEW bad markers    {newbad}")
        regressions.append(f"new bad markers {newbad}")

    p("")
    if regressions:
        p("VERDICT: REGRESSED")
        p("  (frames alone is weak evidence -- see the TOLERANCE note in this"
          " file. deepest_scene, the must-be-zero counters and the bad markers"
          " are the load-bearing checks.)")
        for x in regressions:
            p(f"  - {x}")
        return 1
    p("VERDICT: no regression detected")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("console_log")
    ap.add_argument("--vs", metavar="OLD_LOG",
                    help="diff against an earlier run's console.log")
    ap.add_argument("--json", action="store_true", help="machine-readable summary")
    args = ap.parse_args()

    new = parse(args.console_log)
    if args.json:
        json.dump(summarise(new), sys.stdout, indent=1, default=list)
        print()
        return 0

    rc = 0
    print_report(new)
    if args.vs:
        print("\n" + "=" * 62 + "\n")
        rc = print_diff(new, parse(args.vs))
    return rc


if __name__ == "__main__":
    sys.exit(main())
