#!/usr/bin/env bash
#
# perf.sh -- collect the guest's own PERF records and gate regressions against a
# stored baseline.
#
#   ./perf.sh prog.elf                                  # just report
#   ./perf.sh prog.elf --write-baseline dc/perf/boot.json
#   ./perf.sh prog.elf --baseline dc/perf/boot.json     # gate: exit 1 on regression
#   ./perf.sh prog.elf --baseline B --tolerance 0.05    # 5% band instead of 2%
#   ./perf.sh prog.elf --repeat 3                       # 3 runs, take the median
#   ./perf.sh prog.elf --baseline B --exact acc         # checksum key, must match exactly
#   ./perf.sh prog.elf --baseline B --lower-is-worse fps
#
# JSON on stdout. Exit 0 = no metric regressed outside its band (or no baseline
# was given and at least one PERF record was captured).
#
# ---------------------------------------------------------------------------
# WHY A BAND AND NOT EQUALITY
#
# Flycast's SH-4 timing is scheduler-driven, and the guest measures itself with
# timer_us_gettime64(), so the numbers are very stable but NOT bit-identical:
# the selftest's 1 000 000-iteration loop measured **35085 and 35090 us** across
# runs on this host -- a spread of 5 us, 0.014%. An equality gate would flap.
#
# Default band: **2% relative, with a 100 us absolute floor**, i.e. a metric
# fails only if it moved by more than max(2% of baseline, 100 us). That is ~140x
# the observed jitter, so false failures are implausible, while any change big
# enough to matter for a 30fps frame budget (33 333 us) is caught easily.
# Tighten with --tolerance once a metric has a measured history.
#
# The floor exists because tiny metrics are all noise: 2% of a 40 us
# measurement is 0.8 us, well inside the jitter. Override with --floor.
#
# DIRECTION. By default a metric is "worse when larger" (it is a duration).
# Moving outside the band upward is a REGRESSION and fails; moving outside it
# downward is an IMPROVEMENT and is reported but does not fail unless --strict.
# For metrics where bigger is better (fps, throughput) pass --lower-is-worse KEY.
#
# EXACT KEYS. Some PERF fields are checksums, not measurements (selftest emits
# `acc=1783293664` to prove the loop actually ran). Compare those with --exact
# KEY: any difference at all is a failure. Prefer emitting checksums as MEM or
# ASSERT records so this does not come up.
# ---------------------------------------------------------------------------
#
# WHAT THESE NUMBERS ARE AND ARE NOT (kb/design-harness.md §8d)
#
# They are EMULATED time. Flycast models no cache, no bus contention and no
# store-queue stalls. They are excellent for "did this change make it worse"
# and worthless as an absolute answer to the M3 "<= 25 ms/frame" gate, which
# needs real hardware. Never time the Flycast process from the host instead --
# it runs faster than real time and blocks on serial.
#
# Guest side: accumulate per-frame deltas into a fixed array and print ONE PERF
# line per phase at the end. Never print per frame: at the KOS default baud
# ~190 bytes of logging costs an entire 30fps frame, so the logging would be
# the thing you measured.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
RUNNER="$HERE/_runner.py"

IMAGE="${OC_DC_IMAGE:-$REPO/dc/build/OpenCrossing.cdi}"
RUNROOT="${OC_DC_RUNROOT:-$HOME/.cache/oc-dc-harness/runs}"
TIMEOUT=120
REPEAT=1
TOLERANCE=0.02
FLOOR=100
BASELINE=""
WRITE_BASELINE=""
STRICT=0
RUN_DIR=""
EXACT=()
LOWER_WORSE=()
EXTRA=()

while [ $# -gt 0 ]; do
    case "$1" in
        --baseline)        BASELINE="$2"; shift 2 ;;
        --write-baseline)  WRITE_BASELINE="$2"; shift 2 ;;
        --tolerance)       TOLERANCE="$2"; shift 2 ;;
        --floor)           FLOOR="$2"; shift 2 ;;
        --repeat)          REPEAT="$2"; shift 2 ;;
        --exact)           EXACT+=("$2"); shift 2 ;;
        --lower-is-worse)  LOWER_WORSE+=("$2"); shift 2 ;;
        --strict)          STRICT=1; shift ;;
        --timeout)         TIMEOUT="$2"; shift 2 ;;
        --run-dir)         RUN_DIR="$2"; shift 2 ;;
        -c|--config)       EXTRA+=(-c "$2"); shift 2 ;;
        -h|--help)         sed -n '2,60p' "$0"; exit 0 ;;
        -*)                echo "unknown arg: $1" >&2; exit 2 ;;
        *)                 IMAGE="$1"; shift ;;
    esac
done

case "$IMAGE" in /*) ;; *) IMAGE="$PWD/$IMAGE" ;; esac
[ -f "$IMAGE" ] || { echo "perf: image not found: $IMAGE" >&2; exit 2; }
[ -z "$BASELINE" ]       || case "$BASELINE"       in /*) ;; *) BASELINE="$REPO/$BASELINE" ;; esac
[ -z "$WRITE_BASELINE" ] || case "$WRITE_BASELINE" in /*) ;; *) WRITE_BASELINE="$REPO/$WRITE_BASELINE" ;; esac
[ -n "$BASELINE" ] && [ ! -f "$BASELINE" ] && { echo "perf: no baseline file: $BASELINE" >&2; exit 2; }

STAMP="$(date +%Y%m%d-%H%M%S)"
[ -n "$RUN_DIR" ] || RUN_DIR="$RUNROOT/perf-$(basename "${IMAGE%.*}")-$STAMP-$$"
mkdir -p "$RUN_DIR"

case "$IMAGE" in
    *.cdi|*.gdi|*.chd|*.cue) EXTRA+=(-c config:FastGDRomLoad=yes) ;;
esac

RUNS=()
i=0
while [ "$i" -lt "$REPEAT" ]; do
    d="$RUN_DIR/run$i"
    mkdir -p "$d"
    set +e
    "$RUNNER" "$IMAGE" --run-dir "$d" --timeout "$TIMEOUT" \
        ${EXTRA[@]+"${EXTRA[@]}"} >/dev/null 2>"$d/runner-stderr.log"
    set -e
    RUNS+=("$d/runner.json")
    i=$((i + 1))
done

CFG="$RUN_DIR/perf-config.json"
python3 - "$CFG" "$TOLERANCE" "$FLOOR" "$STRICT" "$BASELINE" "$WRITE_BASELINE" \
         "$IMAGE" "$(IFS=,; echo "${EXACT[*]:-}")" "$(IFS=,; echo "${LOWER_WORSE[*]:-}")" <<'PY'
import json, sys
p, tol, floor, strict, base, wbase, image, exact, lower = sys.argv[1:10]
json.dump({"tolerance": float(tol), "floor": float(floor), "strict": strict == "1",
           "baseline": base or None, "write_baseline": wbase or None, "image": image,
           "exact": [k for k in exact.split(",") if k],
           "lower_is_worse": [k for k in lower.split(",") if k]}, open(p, "w"))
PY

python3 - "$CFG" "${RUNS[@]}" <<'PY'
import json, statistics, sys

cfg = json.load(open(sys.argv[1]))
runs = [json.load(open(p)) for p in sys.argv[2:]]

# --- gather every numeric PERF key from every run --------------------------
samples, statuses = {}, []
for r in runs:
    statuses.append(r.get("status"))
    for rec in r.get("records", {}).get("perf", []):
        for k, v in rec.get("kv", {}).items():
            try:
                n = float(v)
            except ValueError:
                continue
            samples.setdefault(k, []).append(n)

ran_ok = all(s == "ok" for s in statuses) and bool(statuses)

def summarise(v):
    return {"median": statistics.median(v), "min": min(v), "max": max(v),
            "n": len(v), "spread": max(v) - min(v),
            "spread_pct": (100.0 * (max(v) - min(v)) / min(v)) if min(v) else 0.0}

metrics = {k: summarise(v) for k, v in sorted(samples.items())}

out = {
    "harness": "perf",
    "image": cfg["image"],
    "runs": len(runs),
    "run_statuses": statuses,
    "tolerance": cfg["tolerance"],
    "floor": cfg["floor"],
    "metrics": metrics,
    "baseline": cfg["baseline"],
    "comparisons": [],
    "regressions": [],
    "improvements": [],
}

# --- compare against the stored baseline -----------------------------------
ok = ran_ok and bool(metrics)
if not ran_ok:
    out["error"] = "not every run reached its end marker: %s" % statuses
elif not metrics:
    out["error"] = "no numeric PERF records captured -- does the guest print `PERF k=v`?"

if cfg["baseline"] and metrics:
    b = json.load(open(cfg["baseline"]))
    bm = b.get("metrics", {})
    for k, cur in metrics.items():
        if k not in bm:
            out["comparisons"].append({"key": k, "verdict": "new", "current": cur["median"]})
            continue
        ref = bm[k]["median"]
        got = cur["median"]
        delta = got - ref
        pct = (100.0 * delta / ref) if ref else 0.0
        band = max(abs(ref) * cfg["tolerance"], cfg["floor"])
        if k in cfg["exact"]:
            verdict = "match" if got == ref else "changed"
        elif abs(delta) <= band:
            verdict = "within-band"
        else:
            worse_when_smaller = k in cfg["lower_is_worse"]
            worse = (delta < 0) if worse_when_smaller else (delta > 0)
            verdict = "regression" if worse else "improvement"
        c = {"key": k, "baseline": ref, "current": got, "delta": delta,
             "delta_pct": round(pct, 4), "band": band, "verdict": verdict}
        out["comparisons"].append(c)
        if verdict in ("regression", "changed"):
            out["regressions"].append(c)
        elif verdict == "improvement":
            out["improvements"].append(c)
    for k in bm:
        if k not in metrics:
            out["comparisons"].append({"key": k, "verdict": "missing",
                                       "baseline": bm[k]["median"]})
            out["regressions"].append({"key": k, "verdict": "missing",
                                       "baseline": bm[k]["median"]})
    if out["regressions"]:
        ok = False
    if cfg["strict"] and out["improvements"]:
        ok = False

# --- optionally store this run as the new baseline -------------------------
if cfg["write_baseline"] and metrics and ran_ok:
    import os, datetime
    os.makedirs(os.path.dirname(cfg["write_baseline"]) or ".", exist_ok=True)
    json.dump({"image": cfg["image"], "runs": len(runs),
               "recorded_utc": datetime.datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ"),
               "tolerance": cfg["tolerance"], "floor": cfg["floor"],
               "metrics": metrics},
              open(cfg["write_baseline"], "w"), indent=1)
    out["baseline_written"] = cfg["write_baseline"]

out["ok"] = ok
json.dump(out, sys.stdout, indent=1)
sys.stdout.write("\n")

# --- human summary ---------------------------------------------------------
w = sys.stderr.write
for k, m in metrics.items():
    w("PERF %-16s median=%-12g min=%-12g max=%-12g n=%d (spread %.3f%%)\n"
      % (k, m["median"], m["min"], m["max"], m["n"], m["spread_pct"]))
for c in out["comparisons"]:
    if c["verdict"] in ("within-band", "match"):
        continue
    w("  %-12s %-16s baseline=%s current=%s (%+.3f%%)\n"
      % (c["verdict"], c["key"], c.get("baseline"), c.get("current"),
         c.get("delta_pct", 0.0)))
if ok:
    w("DC PERF PASS -- %d metric(s), %d run(s)\n" % (len(metrics), len(runs)))
else:
    if out.get("error"):
        why = out["error"]
    elif out["regressions"]:
        why = "%d regression(s): %s" % (len(out["regressions"]),
                                        ", ".join(r["key"] for r in out["regressions"]))
    else:  # --strict: an improvement is still an unexplained change
        why = "--strict: %d unexpected improvement(s): %s" % (
            len(out["improvements"]), ", ".join(r["key"] for r in out["improvements"]))
    w("DC PERF FAIL -- %s\n" % why)
sys.exit(0 if ok else 1)
PY
