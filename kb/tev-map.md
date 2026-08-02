# TEV → PowerVR CLX2 map — split index

All 101 TEV configurations Animal Crossing actually uses, classified into
fixed-function PVR rendering strategies. Written 2026-08-01 (M2 deliverable,
`PLAN.md` §3.3). Source data: `pc/shaders/shader_seed.bin` — 101
driver-independent `ShaderKey`s harvested from a full playthrough of the ARM
port.

| part | sections | contents |
|---|---|---|
| `kb/tev-map-table.md` | §2, §3, §4 | **the reference table** — global facts, the RGB/alpha class definitions, and all 101 configs with folded polynomial, class and PVR strategy |
| `kb/tev-map-implementation.md` | verdict, §7–§10 | hot classes first, the `PVRDrawMode` fold for the DC GX layer, open items, feasibility verdict |
| `kb/tev-map-alpha.md` | §5 | alpha compare → OP/PT/TR list assignment, blend modes, the global `PT_ALPHA_REF` problem |
| `kb/tev-map-hard-cases.md` | §6 | two-texture configs, out-of-range coefficients, the A3 alpha approximation, the one `GX_TEV_SUB`, blend modes PVR cannot express |
| `kb/tev-map-decoding.md` | §1 | the seed file format, the parser, the folding method, and what the seed cannot tell you |

Marking convention, used throughout every part:
`[F]` = decoded fact (from the seed file or read directly out of the source).
`[I]` = inference. `[?]` = not determinable from available data, needs work.
