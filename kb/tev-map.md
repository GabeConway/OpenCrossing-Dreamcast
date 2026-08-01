# TEV → PowerVR CLX2 map

All 101 TEV configurations Animal Crossing actually uses, classified into
fixed-function PVR rendering strategies. Written 2026-08-01 (M2 deliverable,
`PLAN.md` §3.3). Source data: `pc/shaders/shader_seed.bin` — 101
driver-independent `ShaderKey`s harvested from a full playthrough of the ARM
port.

**Marking convention used throughout:**
`[F]` = decoded fact (from the seed file or read directly out of the source).
`[I]` = inference. `[?]` = not determinable from available data, needs work.

**Verdict up front:** the fixed-function renderer is feasible. **81 / 101
configs (80 %) map onto stock PVR poly modes with no approximation**, another
3 need a trivial approximation, and the residual **17 need a second texture**
— of which 10 are N64 mip-LOD interpolation that can be dropped outright.
Nothing in the set uses indirect textures, non-identity swap tables, texture
matrices, or EFB feedback in the game render path. `[F]`

---

## 1. Where the data comes from, and how it was decoded

### 1.1 File format `[F]`

`pc/src/pc_gx_tev.c` defines the container (`SHADER_DISK_CACHE` block,
lines 588-702) and the key (lines 112-132):

```
header : u32 magic 'ACSC' (0x41435343) | u32 version | u32 driver_hash
entry* : ShaderKey (72 B) | u32 binary_format | u32 length | <length> bytes
```

The seed ships with the blobs stripped (`length == 0`), so every entry is
exactly 80 bytes. Decode check:

```
magic    = 0x41435343 'ACSC'   ✓
version  = 1                   ✓ (matches SDC_VERSION)
filesize = 8092 = 12 + 101×80  ✓  bytes consumed == filesize, 101 entries
all entries: length == 0, _pad == 0   ✓
```

**The file decodes cleanly and the entry count is exactly 101.** No fallback
to static enumeration was needed.

### 1.2 The parser (as used)

```python
#!/usr/bin/env python3
"""Decode pc/shaders/shader_seed.bin -> ShaderKey records.
ShaderKey, verbatim from pc/src/pc_gx_tev.c (all members u8, no padding):
  0  num_stages      1  num_chans
  2  light_enable    3  light_mat_src   4  light_amb_src   (chan_ctrl[0])
  5  alpha_light_en  6  alpha_mat_src                      (chan_ctrl[1])
  7  fog_enabled
  8  alpha_comp0     9  alpha_aop      10  alpha_comp1     11 _pad
  12..71 : 3 x stage[20]:
     +0..3 cin[4] (GXTevColorArg 0-15)   +4..7 ain[4] (GXTevAlphaArg 0-7)
     +8 color_op   +9 alpha_op
     +10 color_bias +11 color_scale +12 alpha_bias +13 alpha_scale
     +14 color_clamp +15 alpha_clamp +16 color_out +17 alpha_out
     +18 k_color_sel +19 k_alpha_sel
"""
import struct
SEED  = "pc/shaders/shader_seed.bin"
MAGIC = 0x41435343

def parse(path=SEED):
    data = open(path, "rb").read()
    magic, ver, drvhash = struct.unpack_from("<III", data, 0)
    assert magic == MAGIC, "bad magic %08x" % magic
    off, keys = 12, []
    while off + 80 <= len(data):
        raw     = data[off:off + 72]
        fmt, ln = struct.unpack_from("<II", data, off + 72)
        off    += 80 + ln                      # ln == 0 in the seed
        k = {}
        (k["num_stages"], k["num_chans"], k["light_en"], k["light_mat_src"],
         k["light_amb_src"], k["alpha_light_en"], k["alpha_mat_src"],
         k["fog"], k["acomp0"], k["aop"], k["acomp1"], k["pad"]) = raw[:12]
        k["s"] = [dict(cin=list(b[0:4]), ain=list(b[4:8]),
                       cop=b[8],    aop=b[9],
                       cbias=b[10], cscale=b[11], abias=b[12], ascale=b[13],
                       cclamp=b[14], aclamp=b[15], cout=b[16], aout=b[17],
                       kc=b[18],    ka=b[19])
                  for b in (raw[12+i*20 : 32+i*20] for i in range(3))]
        keys.append(k)
    return ver, drvhash, keys          # len(keys) == 101
```

### 1.3 How each key was turned into a strategy `[F]` method, `[I]` conclusions

The TEV stage function is `out = d ± lerp(a, b, c)` (± bias, × scale), i.e.
**pure +, −, × over its inputs**. So a whole config is a *polynomial* in two
disjoint symbol sets:

| class | symbols | why it matters on DC |
|---|---|---|
| CPU-known | `C0 C1 C2 A0 A1 A2` (TEV registers, per-draw), `P0 PA0` (initial PREV), `RASC RASA` (lit vertex colour), `KC KA` (konst), rationals | **On DC, lighting runs on SH-4, so `RASC` is computed per vertex on the CPU anyway. Every one of these is a number the CPU already holds at submit time.** |
| per-pixel | `T<m>` / `T<m>a` — rgb / alpha of the texture bound to texmap `m` | only these need the texture unit |

Each config was evaluated with an exact rational polynomial algebra (dict of
sorted-monomial → `Fraction`), then the folded result was matched against
what PVR fixed function can produce:

```python
# PVR pixel = env(base_colour, texel) + offset_colour   (offset alpha ignored)
#   PVR_TXRENV_REPLACE       px = ARGB(tex)
#   PVR_TXRENV_MODULATE      px = A(tex) + RGB(col)*RGB(tex)
#   PVR_TXRENV_DECAL         px = A(col) + RGB(tex)*A(tex) + RGB(col)*(1-A(tex))
#   PVR_TXRENV_MODULATEALPHA px = ARGB(col)*ARGB(tex)
def split_affine(p, T):          # p == Q + C*T with Q, C CPU-known?
    Q, C = Poly(), Poly()
    for mono, coef in p.items():
        occ = [s for s in mono if s in TEXSYM]
        if not occ:        Q[mono] += coef
        elif occ == [T]:   C[mono_without_one(T)] += coef
        else:              return None          # ≥2 texture symbols -> not affine
    return Q, C
```

`base_colour` (`pvr_vertex_t.argb`) and `offset_colour` (`.oargb`) are both
**per-vertex**, so `C` and `Q` may depend on `RASC` — they are emitted by the
same SH-4 transform/lighting loop that already has to run.

### 1.4 The one thing the seed cannot tell you `[?]` → resolved `[F]` for 20 of 101

`ShaderKey` records the combiner *maths* but **not** `tex_map` / `tex_coord`
(they live in `PCGXTevStage` but `build_key()` drops them), and **not** the
alpha-compare reference *values* (`alpha_ref0/1` are uniforms). So the seed
alone cannot say whether stage 0 and stage 1 sample the same texture.

That gap was closed from the source instead: 20 of the 101 configs match
`emu64::combine_manual()` cases (`src/static/libforest/emu64/emu64.c`
1503-1954) exactly on `(num_stages, cin[], ain[])`, and those cases carry
literal `GXSetTevOrder(..., GX_TEXMAP0/1/NULL, ...)` calls. **17 of the 19
configs that reference a second texture slot are in that matched set, so
their texmaps are ground truth, not assumption.** In the table below a
texmap column marked `✓` is ground truth; `?` means the stage index was used
as a proxy (only 2 configs, #004 and #099).

**Action item:** bump `SDC_VERSION` and add `tex_map`, `tex_coord`,
`alpha_ref0`, `alpha_ref1`, `blend_mode/src/dst` and `z_*` to `ShaderKey`,
then re-harvest on the ARM port. That single change turns every `[?]` in this
document into `[F]` and is the cheapest measurement left on the board.

---

## 2. Global facts about the set (all `[F]`, decoded)

| property | value |
|---|---|
| configs | **101** |
| TEV stages | 1 stage: **48**, 2 stages: **45**, 3 stages: **8** (never >3) |
| colour channels | `num_chans == 1` in **all 101** |
| lighting | on in **44**, off in **57** |
| channel control | 3 distinct: (lit, mat=REG, amb=REG, alphaMat=VTX) ×44; (unlit, mat=VTX, alphaMat=VTX) ×50; (unlit, mat=REG, alphaMat=REG) ×7 |
| alpha-channel lighting | **off in all 101** (`chan_ctrl_enable[1] == 0`) |
| fog | on in **43**, off in **58** |
| TEV bias | `GX_TB_ZERO` in **every stage of every config** |
| TEV scale | `GX_CS_SCALE_1` in **every stage of every config** |
| clamp | on in **every stage of every config** |
| output register | `GX_TEVPREV` in **every stage of every config** (R0/R1/R2 are read-only inputs — they are only ever written by `GXSetTevColor`) |
| combiner op | `GX_TEV_ADD` everywhere except **one** `GX_TEV_SUB` (config #086 stage 0) |
| TEV comparison ops (`GX_TEV_COMP_*`) | **never used** |
| `GX_CC_KONST` (colour) | **never used** |
| `GX_CA_KONST` (alpha) | used in **2** configs (#003, #089), and in both the selector resolves to the constant **1.0** — so `GXSetTevKColor` is effectively dead in AC |
| alpha compare | only **3** distinct settings — see §5 |

That the bias/scale/clamp/output-register axes are *completely degenerate*
across a full playthrough is the single most important structural result
here: it collapses the TEV state space from "16 stages × 16 inputs × bias ×
scale × 4 output registers" down to "≤3 chained `d + lerp(a,b,c)` terms into
PREV". `[F]`

### Features PVR lacks — are they used? `[F]`, from call-site enumeration

| GX feature | used in the game render path? | evidence |
|---|---|---|
| **Indirect textures** | **No.** Only `src/static/Famicom/*` (the NES emulator) calls `GXSetNumIndStages(>0)` / `GXSetTevIndirect` / `GXSetIndTexOrder`. The NES core is an explicit non-goal. | `grep GXSetTevIndirect\|GXSetNumIndStages src/` — all non-zero uses are under `src/static/Famicom/`; `pc_gx.c` line 1230 already notes "Indirect textures stripped — shader doesn't use them" and the ARM port renders correctly |
| **TEV swap tables** | **No.** Every non-Famicom call is `GXSetTevSwapMode(stage, GX_TEV_SWAP0, GX_TEV_SWAP0)` and SWAP0 is the identity table `(R,G,B,A)`. | `emu64.c:595-601`, `GXInit.c:324-329`; the only non-identity selections are `ks_nes_draw.cpp:1339-1342` |
| **Texture matrices / texgen** | **No.** emu64 sets `GX_TG_MTX2x4` with `GX_IDENTITY` for every texcoord, and in the 2-texgen path generates **both** TEXCOORD0 and TEXCOORD1 from `GX_TG_TEX0`. | `emu64.c:522-529`, `emu64.c:651-653` |
| **EFB feedback (`GXCopyTex`)** | **One call site in the whole game**: `copy_efb_to_texture()` in `src/game/m_play.c:657-667`, called once from `makeBumpTexture()` (line 787) for the screen-wipe / fade transitions. Full-screen RGB565 copy at 2× res. Not per-frame. | `grep GXCopyTex src/` |

This resolves `PLAN.md` §11 open question 3 in the negative: **no indirect
textures, no EFB feedback in any hot path.** One transition effect needs PVR
render-to-texture, or can be replaced with a plain fullscreen fade.

Corollary `[I]`: because texgen is identity and both texcoords derive from
vertex TEX0, **one UV pair per vertex is enough** — which is exactly what the
32-byte `pvr_vertex_t` provides. No vertex-format pressure from texgen.

---

## 3. Per-class counts

RGB classes (what the folded RGB expression is, in one texture `T`):

| class | meaning | PVR mechanism | count |
|---|---|---|---|
| **P0** | no texture term at all | untextured poly, per-vertex ARGB | **35** |
| **P1** | `= T` | `PVR_TXRENV_REPLACE` | **5** |
| **P2** | `= base·T`, base CPU-known | `PVR_TXRENV_MODULATE` / `MODULATEALPHA` | **29** |
| **P3** | `= base·T + offset`, both CPU-known | `MODULATE` + `oargb` (specular/offset colour enabled) | **27** |
| **P4** | references two texmaps | second pass / bake — §6 | **5** |

Alpha classes:

| class | meaning | PVR mechanism | count |
|---|---|---|---|
| **A0** | constant (per-draw / per-vertex) | poly alpha; env `MODULATE` or `DECAL` (tex alpha ignored) | **15** |
| **A1** | `= T.a` | `REPLACE`, or `MODULATEALPHA` with poly alpha 1.0 | **30** |
| **A2** | `= Af·T.a` | `PVR_TXRENV_MODULATEALPHA` | **36** |
| **A3** | `= Af·T.a + off` | *no native form* — approximate (§6.3) | **3** |
| **A4** | two texmaps or non-affine | second pass / bake / drop — §6 | **17** |

Joint distribution (the actual poly types you have to build):

| RGB \ A | A0 | A1 | A2 | A3 | A4 | total |
|---|---|---|---|---|---|---|
| **P0** | 11 | 2 | 16 | – | 6 | 35 |
| **P1** | – | 5 | – | – | – | 5 |
| **P2** | 2 | 15 | 9 | 3 | – | 29 |
| **P3** | 2 | 8 | 11 | – | 6 | 27 |
| **P4** | – | – | – | – | 5 | 5 |
| **total** | 15 | 30 | 36 | 3 | 17 | **101** |

**Headline numbers**

* **81 / 101 (80.2 %) are natively expressible** — RGB in {P0,P1,P2,P3} and
  alpha in {A0,A1,A2}. One PVR poly, one texture, no approximation.
* **3 / 101** need a one-line approximation (A3).
* **17 / 101 (16.8 %) reference a second texture.** 15 of those 17 have
  ground-truth texmaps `[F]`; 2 are inferred `[?]`.
* **0 / 101** use indirect textures, swap tables, texture matrices, TEV
  comparison ops, bias, scale, or non-PREV output registers.

---

## 4. Full classification table

Columns: `st` = TEV stages, `lit` = colour-channel lighting enabled,
`a-test` = alpha compare (see §5), `tex` = texmaps actually sampled
(`✓` texmap is ground truth from `combine_manual`, `?` inferred from stage
index), `RGB`/`A` = folded polynomial (CPU-known symbols in caps, `T0`/`T0a`
= texel rgb/alpha).

| # | st | fog | lit | a-test | tex | RGB (folded) | A (folded) | class | PVR strategy |
|---|----|-----|-----|--------|-----|--------------|------------|-------|--------------|
| 0 | 2 | 1 | 1 | >=ref1 | T0 | `C1*RASC*T0` | `T0a` | P2/A1 | MODULATE; = tex.a |
| 1 | 2 | 1 | 1 | - | T0 | `C1*RASC*T0` | `T0a` | P2/A1 | MODULATE; = tex.a |
| 2 | 1 | 1 | 0 | - | T0 | `C1` | `A0*T0a` | P0/A2 | untextured, vtx colour; MODULATEALPHA |
| 3 | 1 | 0 | 0 | - | - | `C0` | `KA` | P0/A0 | untextured, vtx colour; poly alpha |
| 4 | 2 | 1 | 1 | - | T0,T1 ? | `T1 + C2*RASC + C1*RASC*T0 - C2*RASC*T0` | `A1 + A0*T0a + A0*T1a` | P4/A4 | **needs 2nd texture** - see §6 |
| 5 | 1 | 0 | 0 | - | T0 | `T0` | `T0a` | P1/A1 | REPLACE; = tex.a |
| 6 | 1 | 0 | 0 | - | - | `C1` | `A1` | P0/A0 | untextured, vtx colour; poly alpha |
| 7 | 2 | 1 | 0 | - | T0,T1 ✓ | `C1` | `A1*T0a*T1a` | P0/A4 | **needs 2nd texture** - see §6 |
| 8 | 2 | 1 | 1 | - | T0 | `C2 + C1*RASC - C2*RASC - A0*C1*RASC + A0*C1*T0 + A0*C2*RASC - A0*C2*T0` | `A1*T0a` | P3/A2 | MODULATE + oargb; MODULATEALPHA |
| 9 | 1 | 0 | 0 | - | T0 | `C1` | `A1*T0a` | P0/A2 | untextured, vtx colour; MODULATEALPHA |
| 10 | 1 | 0 | 0 | >=ref0 | T0 | `C2 + C1*T0 - C2*T0` | `A1*T0a` | P3/A2 | MODULATE + oargb; MODULATEALPHA |
| 11 | 1 | 0 | 0 | - | T0 | `C1` | `T0a` | P0/A1 | untextured, vtx colour; = tex.a |
| 12 | 1 | 0 | 0 | - | T0 | `C1` | `A1*T0a` | P0/A2 | untextured, vtx colour; MODULATEALPHA |
| 13 | 1 | 1 | 1 | - | T0 | `RASC*T0` | `A1*T0a` | P2/A2 | MODULATE; MODULATEALPHA |
| 14 | 1 | 1 | 1 | - | T0 | `C1` | `A0*T0a` | P0/A2 | untextured, vtx colour; MODULATEALPHA |
| 15 | 1 | 1 | 1 | - | - | `C1` | `A1` | P0/A0 | untextured, vtx colour; poly alpha |
| 16 | 2 | 1 | 0 | >=ref1 | T0 | `C1*T0` | `T0a` | P2/A1 | MODULATE; = tex.a |
| 17 | 1 | 1 | 1 | - | - | `C1` | `0` | P0/A0 | untextured, vtx colour; poly alpha |
| 18 | 1 | 1 | 0 | - | - | `C1` | `0` | P0/A0 | untextured, vtx colour; poly alpha |
| 19 | 1 | 1 | 1 | - | T0 | `C2 + C1*RASC` | `A1*T0a` | P0/A2 | untextured, vtx colour; MODULATEALPHA |
| 20 | 3 | 1 | 1 | - | T0,T1 ✓ | `C1*RASC + T0*T1a` | `A1 + A0*T0a*T1a` | P4/A4 | **needs 2nd texture** - see §6 |
| 21 | 2 | 1 | 1 | - | T0 | `C2*RASC + C1*RASC*T0 - C2*RASC*T0` | `0` | P3/A0 | MODULATE + oargb; poly alpha |
| 22 | 2 | 1 | 1 | - | T0 | `C2*RASC + C1*RASC*T0 - C2*RASC*T0` | `T0a` | P3/A1 | MODULATE + oargb; = tex.a |
| 23 | 3 | 1 | 1 | - | T0,T1 ✓ | `T0 + C1*RASC + T0*T1` | `T0a*T1a` | P4/A4 | **needs 2nd texture** - see §6 |
| 24 | 2 | 1 | 1 | - | T0,T1 ✓ | `T0 + C1*RASC + RASC*T0` | `A1*T1a` | P3/A2 | MODULATE + oargb; MODULATEALPHA |
| 25 | 2 | 0 | 0 | - | T0 | `C2*RASC + C1*RASC*T0 - C2*RASC*T0` | `0` | P3/A0 | MODULATE + oargb; poly alpha |
| 26 | 1 | 1 | 1 | - | T0 | `C1*RASC` | `A1*T0a` | P0/A2 | untextured, vtx colour; MODULATEALPHA |
| 27 | 2 | 1 | 1 | - | T0 | `C2*RASC + C1*RASC*T0 - C2*RASC*T0` | `A1*T0a` | P3/A2 | MODULATE + oargb; MODULATEALPHA |
| 28 | 2 | 1 | 1 | - | T0 | `C1*RASC*T0` | `A1*T0a` | P2/A2 | MODULATE; MODULATEALPHA |
| 29 | 1 | 0 | 0 | - | T0 | `RASC*T0` | `A1` | P2/A0 | MODULATE; poly alpha |
| 30 | 2 | 0 | 0 | - | T0,T1 ✓ | `C2 + C1*T0 - C2*T0` | `T1a + T0a*T1a` | P3/A4 | **needs 2nd texture** - see §6 |
| 31 | 2 | 0 | 0 | - | T0 | `C1` | `A0*A1*T0a` | P0/A2 | untextured, vtx colour; MODULATEALPHA |
| 32 | 2 | 0 | 1 | >=ref1 | T0 | `C1*RASC*T0` | `T0a` | P2/A1 | MODULATE; = tex.a |
| 33 | 2 | 0 | 1 | - | T0 | `C1*RASC*T0` | `T0a` | P2/A1 | MODULATE; = tex.a |
| 34 | 1 | 0 | 0 | - | T0 | `C1` | `A1*T0a` | P0/A2 | untextured, vtx colour; MODULATEALPHA |
| 35 | 1 | 0 | 0 | - | T0 | `C2 + C1*T0 - C2*T0` | `A1*T0a` | P3/A2 | MODULATE + oargb; MODULATEALPHA |
| 36 | 2 | 0 | 0 | >=ref1 | T0 | `C1*RASC*T0` | `T0a` | P2/A1 | MODULATE; = tex.a |
| 37 | 1 | 0 | 0 | - | T0 | `C2 + C1*T0 - C2*T0` | `T0a` | P3/A1 | MODULATE + oargb; = tex.a |
| 38 | 2 | 0 | 0 | >=ref1 | T0 ✓ | `C2 + A0*C1 + C2*T0 + A0*C1*T0` | `T0a` | P3/A1 | MODULATE + oargb; = tex.a |
| 39 | 1 | 0 | 0 | - | T0 | `C1*T0` | `T0a` | P2/A1 | MODULATE; = tex.a |
| 40 | 1 | 0 | 0 | - | T0 | `C2 + A0*C1` | `A1*T0a` | P0/A2 | untextured, vtx colour; MODULATEALPHA |
| 41 | 2 | 0 | 0 | - | T0 ✓ | `C2 + A0*C1 + C2*T0 + A0*C1*T0` | `T0a` | P3/A1 | MODULATE + oargb; = tex.a |
| 42 | 2 | 0 | 0 | - | T0,T1 ✓ | `C1` | `A0*T0a*T1a` | P0/A4 | **needs 2nd texture** - see §6 |
| 43 | 1 | 0 | 1 | - | T0 | `C2 + C1*T0 - C2*T0` | `A1*T0a` | P3/A2 | MODULATE + oargb; MODULATEALPHA |
| 44 | 1 | 0 | 0 | - | T0 | `T0` | `T0a` | P1/A1 | REPLACE; = tex.a |
| 45 | 1 | 0 | 0 | >=ref1 | T0 | `C1` | `A1*T0a` | P0/A2 | untextured, vtx colour; MODULATEALPHA |
| 46 | 1 | 0 | 0 | - | T0 | `C1*T0` | `T0a` | P2/A1 | MODULATE; = tex.a |
| 47 | 1 | 0 | 0 | - | - | `C1` | `A1` | P0/A0 | untextured, vtx colour; poly alpha |
| 48 | 1 | 0 | 0 | >=ref1 | T0 | `T0` | `T0a` | P1/A1 | REPLACE; = tex.a |
| 49 | 1 | 0 | 1 | - | T0 | `C1` | `A1*T0a` | P0/A2 | untextured, vtx colour; MODULATEALPHA |
| 50 | 3 | 1 | 1 | - | T0,T1 ✓ | `C1*RASC` | `A1*T0a - A0*A1*T0a + A0*A1*T1a` | P0/A4 | **needs 2nd texture** - see §6 |
| 51 | 1 | 1 | 0 | - | T0 | `C2 + C1*T0 - C2*T0` | `A1*T0a` | P3/A2 | MODULATE + oargb; MODULATEALPHA |
| 52 | 1 | 1 | 1 | - | T0 | `C2 + C1*T0 - C2*T0` | `A1*T0a` | P3/A2 | MODULATE + oargb; MODULATEALPHA |
| 53 | 1 | 0 | 0 | - | - | `C1` | `0` | P0/A0 | untextured, vtx colour; poly alpha |
| 54 | 1 | 0 | 0 | - | - | `C1` | `A1*RASA` | P0/A0 | untextured, vtx colour; poly alpha |
| 55 | 1 | 0 | 0 | - | T0 | `C2 + C1*T0 - C2*T0` | `A1*T0a` | P3/A2 | MODULATE + oargb; MODULATEALPHA |
| 56 | 2 | 1 | 0 | - | T0 | `C1*T0` | `T0a` | P2/A1 | MODULATE; = tex.a |
| 57 | 2 | 0 | 0 | >=ref1 | T0,T1 ✓ | `T0` | `T1a` | P1/A1 | REPLACE; = tex.a |
| 58 | 1 | 0 | 0 | >=ref0 | T0 | `C1 - A0*C1 + A0*T0` | `T0a` | P3/A1 | MODULATE + oargb; = tex.a |
| 59 | 2 | 0 | 1 | - | - | `C1` | `A1` | P0/A0 | untextured, vtx colour; poly alpha |
| 60 | 2 | 0 | 1 | - | T0 | `C1*RASC*T0` | `A1*T0a` | P2/A2 | MODULATE; MODULATEALPHA |
| 61 | 1 | 0 | 0 | >=ref0 | T0 | `C1` | `A1*T0a` | P0/A2 | untextured, vtx colour; MODULATEALPHA |
| 62 | 1 | 0 | 0 | >=ref0 | T0 | `C1` | `T0a` | P0/A1 | untextured, vtx colour; = tex.a |
| 63 | 1 | 0 | 0 | >=ref0 | T0 | `C2 + C1*T0 - C2*T0` | `T0a` | P3/A1 | MODULATE + oargb; = tex.a |
| 64 | 1 | 0 | 0 | >=ref0 | - | `C1` | `A1` | P0/A0 | untextured, vtx colour; poly alpha |
| 65 | 2 | 1 | 1 | - | T0 | `C1*RASC*T0` | `A2*T0a` | P2/A2 | MODULATE; MODULATEALPHA |
| 66 | 2 | 1 | 1 | - | T0 | `C1*RASC*T0` | `A1*A2*T0a` | P2/A2 | MODULATE; MODULATEALPHA |
| 67 | 3 | 1 | 1 | - | T0,T1 ✓ | `C1` | `A1*T0a - A0*A1*T0a + A0*A1*T1a` | P0/A4 | **needs 2nd texture** - see §6 |
| 68 | 3 | 1 | 0 | - | T0,T1 ✓ | `RASC*T0 + C1*C2*RASC - C2*RASC*T0` | `A1*T0a - A0*A1*T0a + A0*A1*T1a` | P3/A4 | **needs 2nd texture** - see §6 |
| 69 | 3 | 1 | 0 | - | T0,T1 ✓ | `C2 + C1*T0*T1a - C2*T0*T1a` | `A1*T0a*T1a` | P4/A4 | **needs 2nd texture** - see §6 |
| 70 | 1 | 1 | 1 | - | - | `C2` | `A1` | P0/A0 | untextured, vtx colour; poly alpha |
| 71 | 2 | 0 | 0 | - | T0 | `C1*RASC*T0` | `T0a` | P2/A1 | MODULATE; = tex.a |
| 72 | 2 | 0 | 0 | - | T0 | `C1*RASC*T0` | `T0a` | P2/A1 | MODULATE; = tex.a |
| 73 | 1 | 0 | 0 | >=ref0 | T0 | `0` | `A2*T0a` | P0/A2 | untextured, vtx colour; MODULATEALPHA |
| 74 | 1 | 0 | 0 | - | T0 | `C2 + C1*T0 - C2*T0` | `A0*T0a` | P3/A2 | MODULATE + oargb; MODULATEALPHA |
| 75 | 3 | 0 | 1 | - | T0,T1 ✓ | `C1*RASC` | `A1*T0a - A0*A1*T0a + A0*A1*T1a` | P0/A4 | **needs 2nd texture** - see §6 |
| 76 | 2 | 1 | 1 | >=ref1 | T0 | `C1*RASC*T0` | `A1 + A0*T0a` | P2/A3 | MODULATE; approx (drop offset) |
| 77 | 2 | 1 | 1 | - | T0,T1 ✓ | `C2 + A0*T1 + C1*RASC` | `T0a*T1a` | P3/A4 | **needs 2nd texture** - see §6 |
| 78 | 2 | 1 | 1 | - | T0,T1 ✓ | `T1 + C1*RASC + RASC*T0` | `A0*T0a*T1a` | P4/A4 | **needs 2nd texture** - see §6 |
| 79 | 2 | 1 | 1 | - | T0,T1 ✓ | `C2 + A0*T0 + C1*RASC` | `T1a + T0a*T1a` | P3/A4 | **needs 2nd texture** - see §6 |
| 80 | 2 | 1 | 1 | - | T0,T1 ✓ | `T0 + C1*RASC + RASC*T0` | `T0a*T1a` | P3/A4 | **needs 2nd texture** - see §6 |
| 81 | 2 | 1 | 1 | >=ref1 | T0 | `C1*RASC*T0` | `A2*T0a` | P2/A2 | MODULATE; MODULATEALPHA |
| 82 | 2 | 1 | 1 | - | T0 | `C1*RASC*T0` | `A2*T0a` | P2/A2 | MODULATE; MODULATEALPHA |
| 83 | 2 | 0 | 0 | - | T0 | `C1*T0` | `T0a` | P2/A1 | MODULATE; = tex.a |
| 84 | 2 | 0 | 0 | >=ref1 | T0 | `C1*T0` | `T0a` | P2/A1 | MODULATE; = tex.a |
| 85 | 1 | 1 | 1 | - | T0 | `C1` | `A1*T0a` | P0/A2 | untextured, vtx colour; MODULATEALPHA |
| 86 | 3 | 1 | 1 | - | T0 ✓ | `1/2*C1*RASC + C2*RASC - C1*RASC*T0` | `A1*T0a` | P3/A2 | MODULATE + oargb; MODULATEALPHA |
| 87 | 2 | 0 | 0 | - | T0 | `C1*RASC*T0` | `A1` | P2/A0 | MODULATE; poly alpha |
| 88 | 2 | 0 | 0 | >=ref1 | T0 | `C1*RASC*T0` | `T0a` | P2/A1 | MODULATE; = tex.a |
| 89 | 2 | 0 | 0 | - | T0 | `RASC*T0` | `A0*KA + A1*T0a` | P2/A3 | MODULATE; approx (drop offset) |
| 90 | 2 | 0 | 0 | - | T0,T1 ✓ | `A1*C1 + A2*T0` | `A0*T0a*T1a` | P3/A4 | **needs 2nd texture** - see §6 |
| 91 | 1 | 0 | 0 | - | T0 | `C1*T0` | `A0*T0a` | P2/A2 | MODULATE; MODULATEALPHA |
| 92 | 1 | 1 | 1 | >=ref1 | T0 | `T0` | `T0a` | P1/A1 | REPLACE; = tex.a |
| 93 | 1 | 0 | 0 | - | T0 | `C1` | `A0*T0a` | P0/A2 | untextured, vtx colour; MODULATEALPHA |
| 94 | 2 | 0 | 1 | - | T0 | `C1*RASC*T0` | `A1*T0a` | P2/A2 | MODULATE; MODULATEALPHA |
| 95 | 1 | 1 | 1 | >=ref1 | T0 | `C1*RASC` | `A1*T0a` | P0/A2 | untextured, vtx colour; MODULATEALPHA |
| 96 | 2 | 1 | 1 | >=ref1 | T0 | `C2*RASC + C1*RASC*T0 - C2*RASC*T0` | `T0a` | P3/A1 | MODULATE + oargb; = tex.a |
| 97 | 2 | 0 | 1 | - | T0 | `C1*RASC*T0` | `A1 + A0*T0a` | P2/A3 | MODULATE; approx (drop offset) |
| 98 | 1 | 0 | 0 | - | T0 | `RASC*T0` | `T0a` | P2/A1 | MODULATE; = tex.a |
| 99 | 2 | 0 | 0 | - | T0,T1 ? | `C1` | `T0a*T1a` | P0/A4 | **needs 2nd texture** - see §6 |
| 100 | 1 | 0 | 0 | >=ref0 | T0 | `C1 - A0*C1 + A0*T0` | `T0a` | P3/A1 | MODULATE + oargb; = tex.a |

---

## 5. Alpha compare → PVR list assignment

### 5.1 What the game actually asks for `[F]`

Only **three** alpha-compare settings occur in the whole 101:

| `(comp0, op, comp1)` | count | effective test |
|---|---|---|
| `(ALWAYS, AND, ALWAYS)` | **78** | no test |
| `(ALWAYS, AND, GEQUAL)` | **15** | `alpha >= ref1` |
| `(GEQUAL, AND, ALWAYS)` | **8** | `alpha >= ref0` |

Both tested forms are `AND`ed with `ALWAYS`, so **every alpha test in Animal
Crossing is a single `alpha >= ref`**. No `LESS`, no `EQUAL`, no `OR`/`XOR`
combination, never both halves active at once. `[F]`

The producer is `emu64::alpha_compare()` (`emu64.c:2299-2321`):

```c
comp0 = (othermode_low & G_AC_DITHER) == G_AC_THRESHOLD ? GX_GEQUAL : GX_ALWAYS;
        ref0 = blend_color.a;                       /* N64 gsDPSetBlendColor  */
comp1 = (AA_EN|CVG_X_ALPHA|ALPHA_CVG_SEL set && !(CVG_DST_SAVE|ZMODE_XLU))
        ? GX_GEQUAL : GX_ALWAYS;
        ref1 = tex_edge_alpha;                      /* default 144 (emu64.c:718),
                                                       overridable per-DL by
                                                       G_SETTEXEDGEALPHA        */
GXSetAlphaCompare(comp0, blend_alpha, GX_AOP_AND, comp1, tex_edge_alpha);
```

So `comp1` is the classic N64 **alpha-to-coverage cutout** (foliage, fences,
sprite edges) and `comp0` is the explicit `G_AC_THRESHOLD` cutout.

### 5.2 What PVR gives you `[F]`

* Punch-through list semantics are exactly `pass iff alpha >= PT_ALPHA_REF`
  (Flycast: `if (cp_AlphaTestValue > color.a) discard;` with
  `cp_AlphaTestValue = (PT_ALPHA_REF & 0xFF)/255`, `core/rend/gles/gles.cpp`
  line 1153 and shader line 345-350). Passing fragments then get `alpha = 1.0`.
* **`PT_ALPHA_REF` is a single global PVR register at `0xA05F811C`**
  (`flycast/core/hw/pvr/pvr_regs.h:79` — "Alpha value for Punch Through
  polygon comparison"). It is **not per-poly**; it is latched for the whole
  render. KOS does not even expose a wrapper for it.
* There is **no alpha test at all** outside the PT list.

### 5.3 The blend axis `[F]` from source, `[?]` in the seed

`ShaderKey` does not record `GXSetBlendMode`, so the *joint* distribution of
(alpha-test × blend mode) over the 101 is unknown. But the producer is a
4-way switch, `emu64::blend_mode()` (`emu64.c:2285-2297`):

| # | condition | GX blend | + `zmode()` side effect |
|---|---|---|---|
| **B1** | `ZMODE_DEC && G_DECAL == (GEQUAL\|SPECIAL)` | `BM_NONE (ONE, ZERO)` | `GXSetColorUpdate(FALSE)` + `GXSetAlphaUpdate(TRUE)` — **alpha-only write** |
| **B2** | `ZMODE_DEC && G_DECAL == SPECIAL` | `BM_BLEND (DSTALPHA, INVDSTALPHA)` | **destination-alpha blend** |
| **B3** | `(IM_RD \| FORCE_BL)` set | `BM_BLEND (SRCALPHA, INVSRCALPHA)` | standard translucency |
| **B4** | otherwise | `BM_NONE` | opaque |

`emu64::zmode()` maps `Z_CMP`/`Z_UPD`/`ZMODE_DEC` onto `GXSetZMode` with
`GX_LESS` normally and `GX_LEQUAL / GX_GEQUAL / GX_EQUAL / GX_ALWAYS` for the
decal modes — all of which have direct `PVR_DEPTHCMP_*` equivalents. `[F]`

### 5.4 List-assignment rules `[I]`, derived from 5.1–5.3

```
                 alpha test?      blend            ->  PVR list          blend_src / blend_dst
B4  opaque       none             none                 OP_POLY           ONE / ZERO
B4  cutout       alpha >= ref     none                 PT_POLY           ONE / ZERO      (+ PT_ALPHA_REF)
B3  translucent  none             SRCALPHA/INVSRC      TR_POLY           SRCALPHA / INVSRCALPHA
B3  cutout+blend alpha >= ref     SRCALPHA/INVSRC      PT_POLY preferred; TR_POLY if the
                                                       ref cannot be honoured (see below)
B1  alpha-mask   either           ONE/ZERO, colour-write off   *** no PVR equivalent — §6.5
B2  dst-alpha    either           DSTALPHA/INVDSTALPHA         *** no reliable PVR equivalent — §6.5
```

Additional rules that fall out of PVR's architecture `[I]`:

1. **Depth-sorted draw order stops mattering for TR.** PVR sorts translucent
   fragments per tile in hardware, so the game's back-to-front submission
   order in the XLU display lists becomes a no-op. Keep submitting in game
   order — it costs nothing and preserves behaviour if OIT is ever disabled.
2. **Do not put alpha-tested geometry in TR.** PT is cheaper and gives correct
   depth interaction with the opaque list. AC's cutouts (foliage, fences,
   the town gate lattice, tree canopies) are the single biggest PT population.
3. **Z-write disable maps directly** (`pvr_poly_cxt_t.depth.write`), needed
   for the B3 translucent path where `Z_UPD` is clear.
4. **`GXSetZCompLoc` is meaningless on PVR** (deferred rasteriser, depth is
   always resolved after the PT alpha test). Ignore it. The GX layer already
   stubs it.

### 5.5 The one genuinely hard alpha-compare problem `[F]` + `[I]`

**`PT_ALPHA_REF` is global; AC's references are per-draw.**
The seed proves two *different* reference registers are in use (`ref0` =
`blend_color.a`, varies per display list; `ref1` = `tex_edge_alpha`, default
144 ≈ 0.565, overridable per-DL) but does **not** record their values `[?]`.

Options, in the order to try them:

* **(a) Pin `PT_ALPHA_REF` to the dominant value and rebake alphas.** Set it
  once (start with 144) and, in the offline texture converter, remap the alpha
  channel of textures used by draws with a different reference so the cutoff
  lands at the pinned value. Alpha is already being requantised to
  ARGB1555 / ARGB4444 / paletted, so this is free. This is the primary plan.
* **(b) Route the outliers to TR.** A cutout rendered as a blended
  translucent poly looks nearly identical (soft rather than hard edge) and
  PVR's OIT keeps it correct. Costs TR fill rate; use for the minority.
* **(c) Two renders with different `PT_ALPHA_REF`.** Correct, and far too
  expensive. Do not.

**Measurement needed before committing** `[?]`: add `alpha_ref0`/`alpha_ref1`
to `ShaderKey` (§1.4) and count how many distinct references actually occur
and with what frequency. If the answer is "144 for 95 % of PT draws" — which
is the expectation, since `G_SETTEXEDGEALPHA` is a rarely-emitted DL command
— option (a) alone is sufficient.

---

## 6. The genuinely problematic configs

### 6.1 Two-texture configs — 17 of 101

These reference two texmaps. Ground truth from `emu64::combine_manual()`
except #004 and #099.

| # | texmaps | folded RGB | folded A | N64 combiner (from `combine_manual`) | strategy |
|---|---|---|---|---|---|
| 007 | T0,T1 ✓ | `C1` (no texture) | `A1*T0a*T1a` | `…TEXEL0, 0, TEXEL1, 0…COMBINED, 0, PRIMITIVE, 0` | **bake**: alpha-only product of two masks |
| 020 | T0,T1 ✓ | `C1*RASC + T0*T1a` | `A1 + A0*T0a*T1a` | `TEXEL0, 0, TEXEL1_ALPHA, 0 … PRIM_LOD_FRAC` | **drop T1** (LOD frac) → `C1*RASC + T0*T0a` → bake premultiplied texture |
| 023 | T0,T1 ✓ | `T0 + C1*RASC + T0*T1` | `T0a*T1a` | `TEXEL1, 0, TEXEL0, TEXEL0 …` | detail-texture modulation; **drop T1** → `T0*(1+T1̄)` with T1̄ folded into base |
| 030 | T0,T1 ✓ | P3 form | `T1a + T0a*T1a` | `…TEXEL0, 0, TEXEL1, TEXEL1 … PRIM_LOD_FRAC` | **drop T1** (LOD frac) |
| 042 | T0,T1 ✓ | `C1` | `A0*T0a*T1a` | `…TEXEL0, 0, TEXEL1, 0 … PRIM_LOD_FRAC` | **drop T1** |
| 050 | T0,T1 ✓ | no texture | `A1*lerp(T0a, T1a, A0)` | `…TEXEL1, TEXEL0, PRIM_LOD_FRAC, TEXEL0 …` | **pure mip-LOD lerp — drop T1**, use `A1*T0a` + PVR trilinear |
| 067 | T0,T1 ✓ | no texture | `A1*lerp(T0a, T1a, A0)` | same family | same |
| 068 | T0,T1 ✓ | P3 form | `A1*lerp(T0a, T1a, A0)` | same family | same |
| 069 | T0,T1 ✓ | `C2 + (C1-C2)*T0*T1a` | `A1*T0a*T1a` | `TEXEL0, 0, TEXEL1_ALPHA, 0 …` | genuine **mask decal**: T1.a masks T0 — second pass, or bake the pair |
| 075 | T0,T1 ✓ | no texture | `A1*lerp(T0a, T1a, A0)` | same LOD family | **drop T1** |
| 077 | T0,T1 ✓ | P3 form | `T0a*T1a` | `…TEXEL0, 0, TEXEL1, 0 … PRIM_LOD_FRAC` | **drop T1** |
| 078 | T0,T1 ✓ | `T1 + C1*RASC + RASC*T0` | `A0*T0a*T1a` | `…TEXEL1, 0, TEXEL0, 0 … PRIM_LOD_FRAC` | **drop T1** → affine in T0 → P3 |
| 079 | T0,T1 ✓ | P3 form | `T1a + T0a*T1a` | `…TEXEL0, 0, TEXEL1, TEXEL1 … PRIM_LOD_FRAC` | **drop T1** |
| 080 | T0,T1 ✓ | P3 form | `T0a*T1a` | `…TEXEL0, 0, TEXEL1, 0 …` | **drop T1** |
| 090 | T0,T1 ✓ | P3 form | `A0*T0a*T1a` | `TEXEL0, 0, ENV_ALPHA, 0 … PRIM_LOD_FRAC` | **drop T1** |
| 004 | T0,T1 ? | `T1 + C2*RASC + (C1-C2)*RASC*T0` | `A1 + A0*T0a + A0*T1a` | (no `combine_manual` match — from `combine_auto`) | if same texmap → collapses to P3; verify with the re-harvest |
| 099 | T0,T1 ? | `C1` (no texture) | `T0a*T1a` | (no match) | alpha-only product; if same texmap → `T0a²`, bake |

**Ten of the seventeen carry the N64 `PRIM_LOD_FRAC` idiom** — TEXEL0 and
TEXEL1 are the *two adjacent mip levels of the same texture* and the combiner
lerps between them by the LOD fraction. That is the N64's software trilinear
filter. PVR does trilinear in hardware (`PVR_FILTER_TRILINEAR1/2`) and
bilinear-with-mipmaps is visually adequate. **The correct DC action is to
delete the second tile entirely and let the texture unit filter.** Cost: zero.
Risk: zero. This alone takes 17 problem configs down to ~7.

The residual genuine multitexture is the **`TEXEL1_ALPHA` mask family**
(#069, #020, and the alpha-product configs #007, #042, #099): one texture's
alpha masks another texture's colour. Two options:
* **Bake the pair offline.** The texture cache is already content-hash keyed
  (`pc_gx_texture.c`); extend the offline converter to emit a premultiplied
  `T0.rgb·T1.a` / `T0.a·T1.a` variant keyed by the *pair* hash. Costs VRAM
  only for the pairs that actually occur.
* **Second pass into the TR list** with `MODULATE` and
  `src=DESTCOLOR, dst=ZERO`, which is exactly what GLdc's
  `GL_ARB_multitexture` already does. Costs a second submission of the
  geometry.
Prefer baking; fall back to the second pass where the pair is dynamic.

### 6.2 Base/offset coefficients that leave [0,1] `[F]` structural, `[I]` impact

`pvr_vertex_t.argb` and `.oargb` are 8-bit per channel, i.e. clamped to
[0,1]. The folded coefficients are not always in range. The table below
covers the **56 configs in classes P2 and P3** (the only ones with a base
coefficient); the other 45 have no coefficient to overflow.

| risk (structural, over the 56 P2/P3 configs) | count | configs |
|---|---|---|
| `base` can go **negative** | 18 | 8, 10, 21, 22, 25, 27, 30, 35, 37, 43, 51, 52, 55, 63, 68, 74, 86, 96 |
| `base` can exceed **1** | 4 | 24, 38, 41, 80 |
| `offset` out of range | 8 | 8, 38, 41, 58, 77, 79, 86, 100 |
| **any** coefficient risk (union) | **26** | the above |
| in range by construction | **30** | all other P2/P3 |

"Structural" means the polynomial's *shape* permits it (a negative
coefficient, or a sum that can exceed 1) — whether it actually happens
depends on the runtime values of `C1`, `C2` and `RASC`, which the seed does
not record. `[I]`

The negative-base cases are all the same shape:
`RGB = C2·RASC + (C1 − C2)·RASC·T`, i.e. `lerp(C2, C1, T)` scaled by the lit
vertex colour — the N64 `PRIMITIVE / ENVIRONMENT / TEXEL0` idiom that AC uses
for ground/terrain tinting and day-night colouring. When `C1 < C2` the base
goes negative and PVR clamps it to 0, which flattens the surface to `C2`.

**Because `C1`, `C2` and `RASC` are all CPU-known at submit time, the choice
of formulation can be made per draw call, not per config.** `[I]` The
submission path should be:

```c
/* base = (C1 - C2)*RASC, offset = C2*RASC  -- per component */
if (all components of base in [0,1] && offset in [0,1])
    MODULATE + oargb;                     /* exact */
else if (C1 < C2 componentwise)
    /* lerp(C2,C1,T) == lerp(C1,C2,1-T): use the offline-inverted texture */
    MODULATE(base = C2-C1) + oargb(C1) with texture T' = 1-T;
else
    clamp and accept the error;           /* or split into two passes */
```

The inverted-texture variant is a cheap offline addition (same twiddle/VQ
pipeline, keyed by content hash + "inverted" flag). The `base > 1` cases
(#024, #038, #041, #080) are of the form `base = 1 + RASC`; halve the base and
double the texture, or take the clamp — 4 configs, low stakes.

### 6.3 The A3 alpha form — 3 configs `[F]`

| # | folded alpha |
|---|---|
| 076 | `A1 + A0*T0a` |
| 097 | `A1 + A0*T0a` |
| 089 | `A0*KA + A1*T0a` (KA resolves to 1.0, so `A0 + A1*T0a`) |

PVR alpha can be `Af`, `At`, or `Af·At` — never `Af·At + off`. Approximation:
**drop the constant offset** and use `MODULATEALPHA` with `Af = A0`. The
offset is an alpha floor (the surface never becomes fully transparent);
losing it makes the surface slightly more transparent at the thin end. Three
configs, low stakes. If it shows, clamp `Af` upward instead:
`Af' = A0 + A1` and accept saturation.

### 6.4 The single `GX_TEV_SUB` — config #086 `[F]`

Matches `combine_manual` case 1, N64 combiner
`NOISE, TEXEL0, PRIMITIVE, ENVIRONMENT, …`:

```
stage0 : PREV.rgb = 0.5 − TEXC          (GX_TEV_SUB)   texmap 0
stage1 : PREV.rgb = C2 + PREV·C1                        no texture
stage2 : PREV.rgb = PREV·RASC                           no texture
folded : RGB = (C2 + C1·(0.5 − T))·RASC
             = (C2 + 0.5·C1)·RASC  −  C1·RASC·T   [verified by the parser]
```

Affine in `T` with a **negative** base — the inverted-texture trick in §6.2
handles it exactly (`T' = 1 − T`, base `= C1·RASC`, offset
`= (C2 − 0.5·C1)·RASC`). One config, one baked texture variant. The `NOISE`
input is already dropped by emu64 itself, so nothing is lost relative to the
ARM port.

### 6.5 Blend modes with no PVR equivalent `[F]` the modes exist, `[I]` the severity

Two of emu64's four blend modes have no clean PVR mapping:

* **B1 — alpha-only write** (`GXSetColorUpdate(FALSE)` + `GXSetAlphaUpdate(TRUE)`,
  blend `ONE/ZERO`). PVR has no colour write mask. This is the N64 "write
  coverage into the framebuffer alpha, then use it as a mask" trick.
* **B2 — destination-alpha blend** (`DSTALPHA / INVDSTALPHA`). PVR does have
  `PVR_BLEND_DESTALPHA` / `INVDESTALPHA` factors, but the value they read is
  the on-chip tile accumulation alpha, and B1 (the pass that is supposed to
  *write* that alpha) cannot run — so the pair is broken as a unit.

Both are gated on `ZMODE_DEC && (geometry_mode & G_DECAL_SPECIAL)` — the N64
**decal** path (ground decals, footprints/shadows projected onto terrain,
puddle overlays). Recommended handling, in order:

1. **Render the decal as a normal PT or TR poly with a small depth bias.**
   PVR's per-poly depth compare plus a z-offset in the SH-4 transform gives
   coplanar decals without the alpha-mask trick. This is what every other DC
   port does.
2. If a decal must be masked by another surface's coverage, bake the mask into
   the decal texture's alpha (the mask geometry is static terrain).
3. Failing both, drop the decal (documented DC-edition cut).

**Unquantified** `[?]`: how many draws per frame take B1/B2. Add
`blend_mode/src/dst` + `color_update`/`alpha_update` to `ShaderKey` in the
re-harvest and count. This is the largest remaining unknown in the renderer
plan — bigger than any TEV combiner question.

---

## 7. Hot paths vs rare configs

**Caveat `[?]`:** the seed carries no draw counts. `shader_cache.bin` is
append-on-first-compile, so seed index ≈ first-encounter order over the
playthrough, which is a *weak* ordering signal, not a frequency measure. The
attributions below are `[I]`, grounded in (a) that ordering, (b) the N64
combiner labels recovered from `combine_manual`, and (c) the folded form.

| likely subsystem | configs | class | why |
|---|---|---|---|
| **Terrain / ground / acre geometry** | the P3 family — `lerp(C2, C1, T)·RASC` (8, 10, 21, 22, 25, 27, 35, 43, 51, 52, 55, 63, 74, 96 …) | P3/A1, P3/A2 | this is the N64 `PRIMITIVE/ENVIRONMENT/TEXEL0` tint idiom; AC recolours ground tiles by season and time of day, and these appear very early in the seed (indices 8–27) with lighting on and fog on |
| **Characters / models** | P2/A1 (0, 1, 16, 32, 33, 36, 39, 46, 56, 71, 72, 83, 84, 88, 98) | P2/A1 | plain `base·tex` with `tex.a`, lighting on, index 0–1 = first thing drawn |
| **Shadows / silhouettes / soft sprites** | P0/A2 (2, 9, 12, 14, 19, 26, 31, 34, 40, 45, 49, 61, 73, 85, 93, 95) — **16 configs** | P0/A2 | flat constant colour, alpha entirely from the texture: the classic blob-shadow / soft-mask draw |
| **UI, fades, letterbox, solid panels** | P0/A0 (3, 6, 15, 17, 18, 47, 53, 54, 59, 64, 70) | P0/A0 | no texture at all, constant alpha; #003 and #006 sit in the `chan_ctrl` "everything from register" group (the 7 unlit/mat=REG/alphaMat=REG configs: 3, 6, 10, 34, 44, 58, 73), which is the JSystem 2D / non-emu64 path |
| **Foliage / fences / cutout edges** | the 15 configs with `a-test = >=ref1` | mixed | `ref1 = tex_edge_alpha`; N64 alpha-to-coverage → **PVR PT list** |
| **Explicit threshold cutouts** | the 8 with `a-test = >=ref0` | mixed | `ref0 = blend_color.a`, `G_AC_THRESHOLD` |
| **Water / animated surfaces** | the `PRIM_LOD_FRAC` two-tile family (50, 67, 68, 75, 77, 79, 80, 90, 020, 030, 042) | P0/P3 + A4 | N64 mip-LOD blending, typical of scrolling water and large ground planes |
| **Rare / one-off** | #086 (`NOISE` combiner, the only `SUB`), #069 + #020 (`TEXEL1_ALPHA` masks), #003 + #089 (the only konst-alpha users) | – | ≤2 configs each |

Practical read `[I]`: the hot classes are **P2/A1, P3/A2, P0/A2 and P0/A0 —
together 62 of 101 configs and, by inspection, the overwhelming majority of
draws.** Every one of them is a stock PVR poly type. The problematic set is
concentrated in mip-LOD blending (droppable) and ground decals (§6.5).

---

## 8. Implementation shape for the DC GX layer

The renderer does **not** need a TEV interpreter. At `pc_gx_flush_vertices`
time, fold the current TEV state to `(base, offset, env_mode, list)` with the
CPU-known values in hand:

```c
typedef struct {
    uint8_t  env;        /* PVR_TXRENV_* */
    uint8_t  list;       /* PVR_LIST_OP_POLY / PT_POLY / TR_POLY */
    uint8_t  specular;   /* oargb enabled (P3 classes) */
    uint8_t  flags;      /* NEEDS_INVERTED_TEX | NEEDS_PREMUL_TEX | SECOND_PASS */
    float    base[4];    /* per-draw part of the base colour; RASC folds in per-vertex */
    float    offset[3];
} PVRDrawMode;
```

* Build a **101-entry lookup table keyed by the same `ShaderKey` hash the ARM
  port already computes** (`hash_key()` in `pc_gx_tev.c`), generated offline
  from this document. Unknown key → fall back to `MODULATE` + vertex colour
  and log it; that is the sm64-dc "wrong colours in places, ships anyway"
  posture, and with 101 known keys it should never fire.
* Per-vertex work in the SH-4 transform loop: compute `RASC` (already
  required — no hardware T&L), multiply into `base`, write `argb`; write
  `oargb` for the 27 P3 configs.
* Texture variants (`inverted`, `premultiplied`) are produced offline by the
  converter and keyed by content hash + variant tag, reusing the existing
  content-hash scheme.
* `PT_ALPHA_REF` is set once per render (§5.5).
* Fog: `k->fog_enabled` selects `PVR_FOG_TABLE` vs `PVR_FOG_DISABLE`
  per poly — a near-native fit, 43 of 101 configs want it.

---

## 9. Open items this document does not close

1. `[?]` **Blend-mode distribution.** How many draws/frame use B1 (alpha-only
   write) and B2 (dst-alpha)? Largest remaining renderer risk (§6.5).
2. `[?]` **Alpha-reference values and their frequency.** Decides whether a
   single global `PT_ALPHA_REF` is viable (§5.5).
3. `[?]` **texmap ground truth for #004 and #099.** Two configs, currently
   assumed.
4. `[?]` **Draw-call frequency per config.** §7 is inference; a counter keyed
   by `ShaderKey` hash on the ARM port would make it fact in one playthrough.
5. All four are answered by the same change: extend `ShaderKey` with
   `tex_map`, `tex_coord`, `alpha_ref0/1`, blend and z state, add a hit
   counter, bump `SDC_VERSION`, re-harvest. **Do this before writing the DC
   renderer.**

---

## 10. Feasibility verdict

**The fixed-function PVR renderer is feasible, and the TEV mismatch is not
the binding constraint on this port.**

* 80 % of configs are exact one-poly one-texture PVR draws.
* The remaining 20 % split into: mip-LOD blends that should simply be deleted
  (10 configs, zero visual cost, PVR filters in hardware), affine forms whose
  coefficients occasionally leave [0,1] and are fixed by an offline inverted
  texture (18 configs, chosen per-draw because the coefficients are
  CPU-known), a 3-config alpha approximation, and 4–5 genuine two-texture
  masks that get baked or a second pass.
* Zero use of the features PVR flatly lacks: indirect textures, swap tables,
  texture matrices, TEV comparison ops, TEV bias/scale, non-PREV output
  registers. One EFB copy in the whole game, used only for screen wipes.
* Alpha compare is a *better* fit than expected: every test in the game is
  `alpha >= ref`, which is precisely PVR punch-through. The only friction is
  that `PT_ALPHA_REF` is global, and that is an offline alpha-rebake problem,
  not an architecture problem.
* The real architectural risks the TEV work surfaced are **not** combiner
  maths: they are (a) emu64's N64 decal path (alpha-only write + destination
  alpha blend), which PVR cannot express and which needs a depth-bias
  substitute, and (b) that all of `RASC` — 8-light GC lighting for 44 of 101
  configs — moves onto the SH-4. Both are already tracked in `PLAN.md`
  §3.2/§3.3.
