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
`zero=0`, no exceptions).

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

# Lines that mean the run went wrong, not merely that it was noisy.
BAD_MARKERS = [
    ("exception", re.compile(r"\[DC/EXC\].*(unhandled|fault|PC=)", re.I)),
    ("assert", re.compile(r"ASSERTION FAILURE|Assertion \".*\" failed")),
    ("oom", re.compile(r"Out of memory|sbrk_base|\bOOM\b")),
    ("short_read", re.compile(r"SHORT READ")),
    ("aram_lost", re.compile(r"LOST=[1-9]")),
]


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
        "bad": dict(r["bad"]),
    }
    return s


# Relative tolerance, per metric, below which a change is NOISE and not a
# verdict. Two runs of the SAME build do not agree: the autostart presses land
# at different points, the camera ends up somewhere else, and the host is
# running a second emulator half the time. Measured spread on identical builds
# is well under 1 % on frames and ~0.1 FPS on the percentiles, so 3 % is loose
# enough never to fire on noise and tight enough to catch a real stall.
#
# Counters in MUST_BE_ZERO get no tolerance at all — 1 is not 0.
TOLERANCE = {
    "frames": 0.03,
    "fps_min": 0.10,   # the minimum is a single worst frame; it is the noisiest
    "fps_p50": 0.03,
    "fps_max": 0.03,
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
}

# Counters with one correct value, whatever the build.
MUST_BE_ZERO = ["pvr_dropped", "pvr_unsupported", "pt_drop", "tex_rejects",
                "aram_lost", "aram_zero"]


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

    p(f"{'metric':<18} {'old':>14} {'new':>14}   delta")
    p("-" * 62)
    regressions = []
    for k, direction in DIRECTION.items():
        ov, nv = b.get(k), a.get(k)
        if ov is None and nv is None:
            continue
        if ov is None or nv is None:
            p(f"{k:<18} {str(ov):>14} {str(nv):>14}   (missing one side)")
            continue
        d = nv - ov
        mark = ""
        tol = TOLERANCE.get(k, 0.0)
        noise = ov and abs(d) <= tol * abs(ov)
        if d and direction:
            good = (d > 0) == (direction > 0)
            if good:
                mark = "  ok"
            elif noise:
                mark = f"  ~noise (<={tol:.0%})"
            else:
                mark = "  REGRESSION"
                regressions.append(f"{k}: {ov} -> {nv}")
        elif d:
            mark = "  changed"
        p(f"{k:<18} {ov:>14,} {nv:>14,}   {d:+,}{mark}"
          if isinstance(ov, (int, float)) and isinstance(nv, (int, float))
          else f"{k:<18} {str(ov):>14} {str(nv):>14}   {mark}")

    p("")
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
