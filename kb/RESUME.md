# RESUME — pick the session back up here

Rewritten 2026-08-02, end of the "the frame is readable" session. Read
`kb/STATE.md` next; this file is only the unfinished part plus the gotchas that
would cost a fresh context an hour.

## 1. Where the port is

**It renders the opening scene correctly.** K.K. Slider, textured, with his
guitar, on a lit stage floor, under a spotlight, with a translucent dialogue
balloon and readable text. 29.3 FPS / 98 % speed. The dialogue advances and the
game goes on to reach a field with the player walking.

That is new. At the start of this session the same frame was two tiled white
wedges over a black screen.

## 2. The build line — use this, do not re-derive it

```bash
DC_STUB_KEEP="$(grep -v '^#' tools/dcstub/keeplist-opening.txt | paste -sd: -)" \
DC_DISC_ROOT=~/.cache/oc-dc-discroot DC_ASSET_STUB=1 \
DC_ARAM_WINDOW=131072 DC_ARENA_BYTES=1900000 DC_AUTOSTART=300 \
  bash dc/build-dc.sh
bash harness/dc/smoke.sh dc/build/OpenCrossing.cdi --timeout 240
```

Add `DC_FB_PROBE=200 DC_FB_IMAGE=2` **and** `--fb-writeback` to get screenshots,
then `python3 tools/dcfb/fbimg_to_png.py <run>/console.log --out /tmp/shots`.

⚠️ **A screenshot run is not a progression run.** `DC_FB_IMAGE` streams ~205 KB
of base64 per frame over a 57600-baud SCIF and eats ~150 s of a 200 s timeout —
the same build reaches **5069 frames without it versus 1379 with it**. Never
judge how far the game gets from a screenshot run.

The trailing `-` in `paste -sd: -` is required; BSD paste on macOS will not read
stdin without it.

## 3. What was fixed this session (do not re-investigate)

All four were the same shape: **state that is recorded and never consumed.**

1. **GX wrap mode** — stored in `TEXOBJ_WRAP_S/T` since M1, never read;
   `dc_pvr.c` hardcoded `PVR_UVCLAMP_NONE`, so every texture repeated. The
   spotlight was drawn 2.7× across the frame.
2. **TEV constant colours** — `g_gx.tev_colors[]` stored by `GXSetTevColor`,
   never read. The opening's shade quad is `(0,0,0,PRIMITIVE)` with
   `PRIM = BLACK`, so a black vignette rendered **white** over 27.9 % of the
   frame. Now 0.0 %.
3. **The keep list** — 77 of 117 texture uploads decoded to all-zero. The
   animals had never been in the image. Re-censused: 31 files / 90 asset loads
   → 76 files / 779.
4. **`.c_inc` files** — invisible to `make_stub_data.py` (globs `*.c`), so the
   dialogue balloon's arrays got a `.bss` buffer and no loader. Both tools now
   handle them.

Narrative and numbers: `kb/state-log.md` top entry. Gotchas: `kb/traps.md`.

## 4. Instrumentation built this session

| knob | what it answers |
|---|---|
| `DC_FB_IMAGE=<1\|2\|4>` + `tools/dcfb/fbimg_to_png.py` | what the frame actually looks like |
| `DC_PVR_BATCH_LOG=<N>` | per-batch tex/wrap/blend/cull/z + emitted screen bbox. Attributes a region of a PNG to the state that drew it |
| `DC_TEX_LOG=1` | what each texture upload *decoded to*. Separates "missing asset" from "renderer bug" — this is what cracked items 3 and 4 |
| `DC_XDEFS='-D...'` | raw defines, so the renderer kill switches are reachable from a command line |

Kill switches: `DC_PVR_NO_UVCLAMP`, `DC_PVR_NO_TEVCONST`, `DC_PVR_NO_CULL`,
`DC_PVR_CULL_INVERT`, `DC_PVR_NO_LIGHTING`, `DC_PVR_NO_NEARCLIP`,
`DC_PVR_NO_TEXTURES`. See `BUILDING-DC.md`.

## 5. Open, in priority order

1. **11 texture uploads still decode blank** — 64×64/128×64 acre and scenery
   textures that this indoor scene loads but never draws. Fix by censusing a
   run that reaches the town, then regenerating `keeplist-opening.txt`.
2. **The near-plane clipper has never fired.** `tris in == out`, `clipped=0`,
   `dropped=0` cumulatively over 623k triangles. For a camera inside geometry
   that is implausible — suspect the projection never produces `w <= EPS`.
   Same class of convention error as the cull and wrap bugs.
3. **A set of quad draws produces nothing visible.** Quads alternate 50 → 3
   between frames whose pixel diff is 1.2 % of scattered edge noise.
4. **The SE slot table never frees** (`[TRG_SE] NO FREE` / `EVICT` every sound).
   `f=` ages climb monotonically. Real bug in the DC audio layer; not blocking.
5. **TEV proper.** Only the `a=b=c=ZERO, d=<const>` shape is handled out of the
   101 configs in `kb/tev-map.md`.
6. **`DC_SRC_SHRINK=0` is broken** — renders nothing (`batches=23 draws=0`).
   The lever stays on; worth knowing it is not a valid A/B control.

## 6. Environment

The auto-mode classifier that blocked every `docker` command last session is
**gone** — a fresh context cleared it, no `/permissions` change needed. Docker,
the SDK image, the build and the harness all work.

**Nothing is committed.** `git status` shows the working tree; the main thread
commits, agents do not.
